#!/usr/bin/env python3
"""Corrupting UDP proxy for exercising the checksum-mismatch branch on loopback."""
#
# Like lossy_proxy.py this runs two unidirectional proxies in one process:
#
#   * Forward direction:  listen-fwd  -> target-fwd  (sender -> receiver path)
#   * Backward direction: listen-back -> target-back (receiver -> sender path)
#
# Unlike the lossy proxy it drops nothing. Instead, exactly ONE TRANSFER packet
# in the forward direction has a single payload byte flipped; everything else is
# forwarded byte-for-byte. The part still validates (its header, index and length
# fields are untouched, so the receiver accepts and never re-requests it), but the
# reassembled file no longer matches the announced SHA-256 — driving the receiver
# into verifyAndWrite's "checksum mismatch — received file is corrupt" branch.
#
# Only the first data-bearing TRANSFER is corrupted, so the outcome is fully
# deterministic regardless of how the OS schedules the datagrams.

import argparse
import socket
import sys
import threading

# Wire layout mirrored from src/Protocol.hpp (protocol v3):
#   magic "FCST"(4) + version(1) + type(1) + session(4) = 10-byte header,
#   then TRANSFER adds part(4) + length(4) before the payload.
MAGIC = b"FCST"
TYPE_TRANSFER = 2
TRANSFER_HEADER = 18  # HEADER_SIZE(10) + part(4) + length(4); payload starts here


def is_transfer(data):
    """True if data is a data-bearing v3 TRANSFER packet (right magic and type)."""
    return len(data) > TRANSFER_HEADER and data[:4] == MAGIC and data[5] == TYPE_TRANSFER


def forward_clean(listen_port, target_host, target_port):
    """Forward every datagram from listen_port to the target byte-for-byte."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", listen_port))
    while True:
        data, _ = sock.recvfrom(65536)
        sock.sendto(data, (target_host, target_port))


def forward_corrupt(listen_port, target_host, target_port):
    """Forward datagrams, flipping one payload byte of the first TRANSFER seen."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", listen_port))
    corrupted = False
    while True:
        data, _ = sock.recvfrom(65536)
        if not corrupted and is_transfer(data):
            part = int.from_bytes(data[10:14], "big")
            buf = bytearray(data)
            buf[TRANSFER_HEADER] ^= 0xFF  # flip one payload byte, leave framing intact
            data = bytes(buf)
            corrupted = True
            print(f"[corrupt] flipped one payload byte of TRANSFER part={part}",
                  file=sys.stderr)
        sock.sendto(data, (target_host, target_port))


def main():
    """Parse CLI arguments and run the two forwarding threads until killed."""
    p = argparse.ArgumentParser()
    p.add_argument("--listen-fwd",  type=int, required=True,
                   help="Listen port for the sender->receiver direction")
    p.add_argument("--target-fwd",  type=int, required=True,
                   help="Where to forward sender->receiver traffic (corrupted once)")
    p.add_argument("--listen-back", type=int, required=True,
                   help="Listen port for the receiver->sender direction")
    p.add_argument("--target-back", type=int, required=True,
                   help="Where to forward receiver->sender traffic (untouched)")
    args = p.parse_args()

    threading.Thread(
        target=forward_corrupt,
        args=(args.listen_fwd, "127.0.0.1", args.target_fwd),
        daemon=True,
    ).start()
    threading.Thread(
        target=forward_clean,
        args=(args.listen_back, "127.0.0.1", args.target_back),
        daemon=True,
    ).start()

    threading.Event().wait()  # block forever; killed by parent


if __name__ == "__main__":
    main()
