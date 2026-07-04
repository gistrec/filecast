# Filecast wire protocol (v2)

Four packet types over UDP. All multi-byte fields are big-endian. Every packet
carries a 32-bit session id chosen randomly by the sender, so a receiver
ignores traffic from a different (or restarted) sender.

## Packet layouts

| Packet | Layout |
| ------ | ------ |
| `NEW_PACKET` | `"NEW_PACKET"` · version(2) · session(4) · file_size(4) · chunk_size(4) · sha256(32) · name_len(2) · name |
| `TRANSFER` | `"TRANSFER"` · session(4) · part(4) · length(4) · data |
| `FINISH` | `"FINISH"` · session(4) |
| `RESEND` | `"RESEND"` · session(4) · part(4) |

## Transfer sequence

1. The sender broadcasts a `NEW_PACKET` announcing a random session id, the
   total file size, the chunk size, the file's SHA-256, and its name.
2. Each receiver latches that session, allocates a buffer, and clears its part
   registry. Packets from any other session are ignored. The chunk size is
   taken from the announcement, so sender and receivers never disagree about
   part boundaries even with mismatched `--mtu` settings.
3. The sender splits the file into chunk-sized pieces and broadcasts each one
   as a `TRANSFER` packet, tagged with the session id, at the configured rate.
4. The sender broadcasts a `FINISH` packet when all chunks have been sent.
5. Each receiver scans for missing chunks and requests them with `RESEND`
   packets.
6. The sender retransmits each requested chunk.
7. Steps 5–6 repeat until every chunk is received or the TTL expires.
8. The receiver recomputes the SHA-256 of the reassembled file and only writes
   it out (atomically, via a temp file and rename) if the digest matches;
   otherwise it reports corruption and fails.

Because every `TRANSFER` is addressed to the broadcast/multicast group rather
than to individual receivers, the cost of a transfer does not grow with the
number of receivers — and a `RESEND` requested by one receiver repairs that
part for everyone who missed it.

## Protocol versioning

The version field in `NEW_PACKET` is checked by receivers: an announcement
carrying an unknown version is reported and ignored. Announcements from the
pre-v2 protocol have no version field at all and are shorter than the v2
fixed header, so they are silently ignored as truncated. The v2 format
(SHA-256, in-band file name, negotiated chunk size) is not compatible with
the v1 format used before release v1.0.0.
