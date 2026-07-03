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
- Unicast mode for point-to-point transfer
- Automatic retransmission of lost packets
- Configurable MTU and timeout
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
./filecast receive photo.jpg
```

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
filecast receive [file] [options]    # receive a file (default: file.out)
```

## Parameters

| Parameter | Default | Range | Description |
| --------- | ------- | ----- | ----------- |
| `<file>` (positional) | — (send) / `file.out` (receive) | — | File to send, or where to save it. `-f, --file` is an alias |
| `--to`        | broadcast  | IPv4 | Send to one host instead of LAN broadcast |
| `-p, --port`  | `33333`    | 1..65535 | Destination port for outgoing packets |
| `--bind-port` | `33333`    | 1..65535 | Local port to bind on |
| `--mtu`       | `1500`     | 64..65507 | Max packet size in bytes |
| `--ttl`       | `15`       | > 0 | Seconds of silence before giving up |
| `--delay-ms`  | `20`       | ≥ 0 | Pause between successive packets (use `0` for loopback / tests) |
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

1. Sender broadcasts a `NEW_PACKET` packet with the total file size.
2. Each receiver allocates a buffer of that size and clears its part registry.
3. Sender splits the file into MTU-sized chunks and broadcasts each one as a
   `TRANSFER` packet.
4. Sender broadcasts a `FINISH` packet when all chunks have been sent.
5. Each receiver scans for missing chunks and requests them with `RESEND`
   packets.
6. Sender retransmits each requested chunk.
7. Steps 5–6 repeat until every chunk is received or the TTL expires.

## Packet Structure

![Packet structure](https://www.gistrec.ru/wp-content/uploads/2019/01/Packets.png)

## Limitations

- The whole file is held in RAM on both sides. The receiver enforces a 4 GiB
  cap on the announced file size; the sender is bounded only by available
  memory.
- No data integrity check beyond UDP's optional 16-bit checksum. If the
  payload is corrupted in a way the checksum doesn't catch, the receiver will
  silently produce a corrupted file.
- No authentication. Any host on the same LAN can send a `NEW_PACKET` and any
  receiver bound to the chosen port will accept it.
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
