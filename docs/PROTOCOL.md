# Filecast wire protocol (v3)

Four packet types over UDP. All multi-byte fields are big-endian.

## Common header

Every packet starts with the same 10-byte header:

| Field | Size | Value |
| ----- | ---- | ----- |
| magic | 4 | `"FCST"` |
| version | 1 | `3` |
| type | 1 | `1` ANNOUNCE · `2` TRANSFER · `3` FINISH · `4` RESEND |
| session | 4 | random id chosen by the sender per transfer |

The magic keeps stray UDP traffic on the port from being misparsed (and is
still recognizable to a human reading a tcpdump hex dump). The session id is
stamped into every packet so a receiver ignores traffic from a different (or
restarted) sender. The version travels in every packet — a receiver that sees
the filecast magic with an unknown version reports it once and ignores the
traffic, so a version-mixed LAN fails loudly instead of silently. The header
layout itself is the compatibility contract: future protocol versions keep
these 10 bytes.

## Packet bodies

| Packet | Body (after the common header) |
| ------ | ------------------------------ |
| `ANNOUNCE` | file_size(4) · chunk_size(4) · sha256(32) · name_len(2) · name |
| `TRANSFER` | part(4) · length(4) · data |
| `FINISH` | — |
| `RESEND` | part(4) |

## Transfer sequence

1. The sender broadcasts an `ANNOUNCE` carrying a random session id, the total
   file size, the chunk size, the file's SHA-256, and its name.
2. Each receiver latches that session, creates an on-disk `.part` file, and
   clears its part registry. The chunk size is taken from the announcement, so
   sender and receivers never disagree about part boundaries even with
   mismatched `--mtu` settings.
3. The sender splits the file into chunk-sized pieces and broadcasts each one
   as a `TRANSFER` packet at the configured rate.
4. The sender broadcasts `FINISH` when all chunks have been sent.
5. Each receiver scans for missing chunks and requests them with `RESEND`
   packets.
6. The sender retransmits each requested chunk.
7. Steps 5–6 repeat until every chunk is received or the TTL expires.
8. The receiver recomputes the SHA-256 of the reassembled file and only writes
   it out (atomically, via a temp file and rename) if the digest matches;
   otherwise it reports corruption and fails.

The sender reads the source file from disk when hashing for `ANNOUNCE` and
again when sending or resending chunks. The source file must remain unchanged
for the full transfer; a concurrent modification makes the transferred bytes no
longer match the announced digest.

Because every `TRANSFER` is addressed to the broadcast/multicast group rather
than to individual receivers, the cost of a transfer does not grow with the
number of receivers — and a `RESEND` requested by one receiver repairs that
part for everyone who missed it.

## Protocol versioning

A packet carrying the filecast magic with any version other than 3 is dropped,
and the receiver (or the sender, for RESENDs) prints a single warning naming
both versions. Packets without the magic — including packets from the
pre-release v1/v2 formats, which used text markers instead of this header —
are silently ignored as unrelated traffic.
