# Filecast

<p align="left">
    <a href="https://github.com/gistrec/filecast/actions/workflows/tests.yml">
        <img src="https://github.com/gistrec/filecast/actions/workflows/tests.yml/badge.svg" alt="Tests"></a>
    <a>
      <img src="https://img.shields.io/codacy/grade/4c8169bcab3a4df18baad4e5658ec8ce" alt="Code quality"></a>
    <a href="https://github.com/gistrec/filecast/releases">
        <img src="https://img.shields.io/github/v/release/gistrec/filecast" alt="Release"></a>
    <a>
      <img src="https://img.shields.io/badge/platform-windows%20%7C%20linux%20%7C%20macos-brightgreen" alt="Platform"></a>
    <a href="https://github.com/gistrec/filecast/blob/master/LICENSE">
        <img src="https://img.shields.io/github/license/gistrec/filecast?color=brightgreen" alt="License"></a>
</p>

UDP broadcast file transfer — sends a single file to every host on the same
LAN at once, with automatic retransmission of dropped packets.

## Table of Contents

- [Features](#features)
- [Quick Start](#quick-start)
- [Installation](#installation)
- [Parameters](#parameters)
- [Examples](#examples)
- [How It Works](#how-it-works)
- [Packet Structure](#packet-structure)
- [Limitations](#limitations)
- [Building from Source](#building-from-source)
- [License](#license)

## Features

- Broadcast to every host on a LAN with a single transmission
- Unicast mode for point-to-point transfer, and IP multicast for one-to-many
  without flooding the whole VLAN
- Automatic retransmission of lost packets
- End-to-end SHA-256 integrity check — a corrupted file is rejected, not saved
- File name travels with the transfer, so `receive` needs no arguments
- Chunk size negotiated in-band, so mismatched `--mtu` no longer stalls
- Live progress bar with speed and ETA, and a rate limit in Mbit/s
- Never clobbers an existing file unless you pass `--overwrite`
- Resumable: an interrupted receive can pick up where it left off with `--resume`
- Windows, Linux, and macOS binaries built on every release

## Quick Start

Download the binary for your platform from the
[releases page](https://github.com/gistrec/filecast/releases) and run it
directly. No installation required.

**Sender** (host that has the file):

```sh
./filecast send photo.jpg
```

**Receiver** (one or more hosts on the same LAN):

```sh
./filecast receive
```

The file is saved under the name the sender announced (pass a path to override
it, e.g. `./filecast receive my-photo.jpg`).

Send to a specific host instead of broadcasting to the whole LAN:

```sh
./filecast send photo.jpg --to 192.168.1.50
```

## Installation

Pre-built binaries for Linux x86_64, macOS arm64, and Windows x86_64 are
attached to every [GitHub Release](https://github.com/gistrec/filecast/releases).

If your platform isn't covered, see [Building from Source](#building-from-source).

## Usage

```sh
filecast send <file> [options]       # broadcast a file to the LAN
filecast receive [file] [options]    # receive a file (default: name from sender)
```

## Parameters

| Parameter | Default | Range | Description |
| --------- | ------- | ----- | ----------- |
| `<file>` (positional) | — (send) / name from sender (receive) | — | File to send, or where to save it. `-f, --file` is an alias |
| `--to`        | broadcast  | IPv4 | Send to one host instead of LAN broadcast |
| `--multicast` | broadcast  | IPv4 multicast | Use an IP multicast group (224.0.0.0-239.255.255.255) instead of broadcast |
| `--iface`     | system-chosen | IPv4 | Multicast interface — the local NIC's IPv4 to send/receive the group on (`--multicast` only) |
| `-p, --port`  | `33333`    | 1..65535 | Destination port for outgoing packets |
| `--bind-port` | `33333`    | 1..65535 | Local port to bind on |
| `--mtu`       | `1500`     | 64..65507 | Max packet size in bytes |
| `--ttl`       | `15`       | > 0 | Seconds of silence before giving up |
| `--rate`      | `100`      | > 0 | Target send rate in Mbit/s |
| `--overwrite` | off        | — | Overwrite an existing output file |
| `--resume`    | off        | — | Resume an interrupted receive from its `.part` snapshot |
| `-v, --verbose` | off      | — | Log every packet instead of a progress bar |
| `--delay-ms`  | —          | ≥ 0 | Advanced: fixed inter-packet pause in ms; overrides `--rate` (`0` blasts at full speed, used by tests) |
| `-h, --help`  | —          | — | Print help |
| `--version`   | —          | — | Print version |

## Examples

**LAN broadcast** (one sender, many receivers):

```sh
# On the sender host
./filecast send album.zip

# On every receiver host
./filecast receive album.zip
```

**Targeted unicast** (when broadcast is blocked or you only have one receiver):

```sh
# On the sender host (sends data to 10.0.0.42)
./filecast send album.zip --to 10.0.0.42

# On 10.0.0.42 (receiver broadcasts its RESENDs by default)
./filecast receive album.zip
```

**IP multicast** (one-to-many without flooding every host on the VLAN — NICs of
non-members drop the traffic in hardware and IGMP-snooping switches forward it
only to subscribers). Sender and every receiver use the same group:

```sh
# On the sender host
./filecast send album.zip --multicast 239.1.2.3

# On every receiver host (same group)
./filecast receive album.zip --multicast 239.1.2.3
```

On a multi-homed host (several NICs), pin the group to a specific interface by
its local IPv4 so the kernel doesn't pick the wrong one:

```sh
./filecast send album.zip --multicast 239.1.2.3 --iface 192.168.1.10
./filecast receive album.zip --multicast 239.1.2.3 --iface 192.168.1.10
```

**Loopback test** (sender and receiver on the same host — useful for
development):

```sh
# Receiver listens on 33401, sends RESEND back to the sender's bind port (33402)
./filecast receive out.bin \
           --to 127.0.0.1 --port 33402 --bind-port 33401 &

# Sender listens on 33402, sends data to the receiver's bind port (33401)
./filecast send in.bin \
           --to 127.0.0.1 --port 33401 --bind-port 33402
```

## How It Works

1. Sender broadcasts a `NEW_PACKET` announcing a random session id, the total
   file size, the chunk size, the file's SHA-256, and its name.
2. Each receiver latches that session, allocates a buffer, and clears its part
   registry. Packets from any other session are ignored.
3. Sender splits the file into chunk-sized pieces and broadcasts each one as a
   `TRANSFER` packet, tagged with the session id.
4. Sender broadcasts a `FINISH` packet when all chunks have been sent.
5. Each receiver scans for missing chunks and requests them with `RESEND`
   packets.
6. Sender retransmits each requested chunk.
7. Steps 5–6 repeat until every chunk is received or the TTL expires.
8. The receiver recomputes the SHA-256 of the reassembled file and only writes
   it out if the digest matches; otherwise it reports corruption and fails.

## Packet Structure

All multi-byte fields are big-endian. Every packet carries the 32-bit session id
so a receiver ignores traffic from a different (or restarted) sender.

| Packet | Layout |
| ------ | ------ |
| `NEW_PACKET` | `"NEW_PACKET"` · version(2) · session(4) · file_size(4) · chunk_size(4) · sha256(32) · name_len(2) · name |
| `TRANSFER` | `"TRANSFER"` · session(4) · part(4) · length(4) · data |
| `FINISH` | `"FINISH"` · session(4) |
| `RESEND` | `"RESEND"` · session(4) · part(4) |

## Resuming an Interrupted Transfer

If a receive is interrupted with Ctrl+C, or times out with parts still missing,
the receiver saves what it has to `<name>.part` (plus a `<name>.part.idx` record
of which parts arrived). Re-run with `--resume` and it picks up where it left
off — the transfer is matched by the file's SHA-256, so it works even if the
sender is restarted (a new session):

```sh
./filecast receive album.zip --resume
```

The snapshot is deleted once the file completes and its checksum verifies.

## Limitations

- The whole file is held in RAM on both sides. The receiver enforces a 4 GiB
  cap on the announced file size; the sender rejects files that do not fit the
  4-byte wire size field.
- `--resume` recovers from Ctrl+C and timeouts (the snapshot is written on exit);
  a hard kill (SIGKILL) or power loss mid-transfer can still lose the in-flight
  progress. The snapshot is the whole buffer written synchronously, so
  interrupting a multi-gigabyte transfer adds a short exit delay while it flushes.
  The `.part`/`.part.idx` files use stable, predictable names in the working
  directory, so run the receiver from a directory only you can write to.
- No authentication. Any host on the same LAN can send a `NEW_PACKET` and any
  receiver bound to the chosen port will accept it. The SHA-256 check catches
  accidental corruption, not a deliberately crafted stream.
- No encryption. The payload travels as plaintext UDP.

## Building from Source

### Requirements

- CMake 3.15+
- A C++17 compiler:
  - GCC 7+ or Clang 5+ on Linux/macOS,
  - MinGW64 GCC via [MSYS2](https://www.msys2.org/) on Windows,
  - or MSVC 2019+ through the Visual Studio CMake generator.
- pthreads (Linux/macOS).

### Build

```sh
git clone https://github.com/gistrec/filecast.git
cd filecast
git submodule update --init --recursive
cmake -S . -B build
cmake --build build --config Release
```

The binary lands at `build/filecast` (or `build\Release\filecast.exe` with the
multi-config Visual Studio generator).

### Tests

```sh
ctest --test-dir build --output-on-failure
```

Runs the unit tests, the loopback end-to-end test, and a lossy variant that
drops packets through a Python UDP proxy to exercise the RESEND branch. Pass
`-E e2e` to skip the e2e cases on Windows, where Winsock semantics break
two-process loopback.

## License

[MIT](LICENSE).
