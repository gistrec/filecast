#!/usr/bin/env python3
"""Session-hijack UDP proxy for exercising the same-session ANNOUNCE guard."""
#
# Like corrupt_proxy.py, forwards every real packet byte-for-byte but models a LAN
# attacker who sniffed the cleartext session id: after the first TRANSFER it
# injects ONE forged ANNOUNCE that reuses the session id (so it passes the
# different-session gate) yet claims a different file_length/sha256/name. Fired
# exactly once, so the outcome is deterministic.

import argparse
import socket
import sys
import threading

# Wire layout mirrored from src/Protocol.hpp (protocol v3):
#   magic "FCST"(4) + version(1) + type(1) + session(4) = 10-byte header.
# ANNOUNCE then adds file_size(4) + chunk_size(4) + sha256(32) + name_len(2),
# and finally the variable-length name.
MAGIC = b"FCST"
TYPE_ANNOUNCE = 1
TYPE_TRANSFER = 2
HEADER_SIZE = 10
ANNOUNCE_FIXED = HEADER_SIZE + 42  # header + size(4) + chunk(4) + hash(32) + name_len(2)

# The forged file the attacker claims. Length/chunk are overridable so the test
# can also forge values announceValid() rejects (empty file, out-of-range chunk),
# which the guard must drop before they become a fatal error.
FORGED_NAME = b"evil"
FORGED_HASH = bytes([0xAB]) * 32


def is_type(data, wire_type):
    """True if data is one of our v3 packets of the given type."""
    return len(data) >= HEADER_SIZE and data[:4] == MAGIC and data[5] == wire_type


def build_forged_announce(session, forged_length, forged_chunk):
    """An ANNOUNCE reusing `session` but describing a different (maybe invalid) file."""
    pkt = bytearray(ANNOUNCE_FIXED + len(FORGED_NAME))
    pkt[0:4] = MAGIC
    pkt[4] = 3  # VERSION
    pkt[5] = TYPE_ANNOUNCE
    pkt[6:10] = session
    pkt[10:14] = forged_length.to_bytes(4, "big")   # file_size
    pkt[14:18] = forged_chunk.to_bytes(4, "big")    # chunk_size
    pkt[18:50] = FORGED_HASH                         # sha256
    pkt[50:52] = len(FORGED_NAME).to_bytes(2, "big")  # name_len
    pkt[52:52 + len(FORGED_NAME)] = FORGED_NAME
    return bytes(pkt)


def forward_and_hijack(listen_port, target_host, target_port, forged_length, forged_chunk):
    """Forward sender->receiver traffic, injecting one forged ANNOUNCE mid-flight."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", listen_port))
    session = None
    injected = False
    while True:
        data, _ = sock.recvfrom(65536)
        if session is None and is_type(data, TYPE_ANNOUNCE):
            session = data[6:10]  # sniff the cleartext session id
        # Forward the real packet untouched first, so the receiver has latched the
        # session and stored at least one real part before the forgery lands.
        sock.sendto(data, (target_host, target_port))
        if not injected and session is not None and is_type(data, TYPE_TRANSFER):
            forged = build_forged_announce(session, forged_length, forged_chunk)
            sock.sendto(forged, (target_host, target_port))
            injected = True
            print("[hijack] injected forged same-session ANNOUNCE "
                  f"(session={int.from_bytes(session, 'big'):#010x}, "
                  f"claimed length={forged_length}, chunk={forged_chunk})", file=sys.stderr)


def forward_clean(listen_port, target_host, target_port):
    """Forward every datagram from listen_port to the target byte-for-byte."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", listen_port))
    while True:
        data, _ = sock.recvfrom(65536)
        sock.sendto(data, (target_host, target_port))


def main():
    """Parse CLI arguments and run the two forwarding threads until killed."""
    p = argparse.ArgumentParser()
    p.add_argument("--listen-fwd",  type=int, required=True,
                   help="Listen port for the sender->receiver direction")
    p.add_argument("--target-fwd",  type=int, required=True,
                   help="Where to forward sender->receiver traffic (forged ANNOUNCE injected)")
    p.add_argument("--listen-back", type=int, required=True,
                   help="Listen port for the receiver->sender direction")
    p.add_argument("--target-back", type=int, required=True,
                   help="Where to forward receiver->sender traffic (untouched)")
    p.add_argument("--forge-length", type=int, default=4096,
                   help="file_size claimed by the forged ANNOUNCE (0 = empty, invalid)")
    p.add_argument("--forge-chunk", type=int, default=1400,
                   help="chunk_size claimed by the forged ANNOUNCE (out of [64,65507] = invalid)")
    args = p.parse_args()

    threading.Thread(
        target=forward_and_hijack,
        args=(args.listen_fwd, "127.0.0.1", args.target_fwd,
              args.forge_length, args.forge_chunk),
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
