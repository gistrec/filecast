#!/usr/bin/env python3
#
# One-directional UDP drop-proxy for the recovery-phase flood test
# (tests/e2e_flood.sh). Listens on IN and forwards every datagram to OUT, except
# it permanently drops TRANSFER (type=2) packets whose part index % MOD == 0. The
# dropped parts never reach the receiver and can never heal, so the receiver is
# forced into — and kept in — the RESEND recovery phase. ANNOUNCE, FINISH, and
# everything else are forwarded untouched.
#
#   dropproxy.py <in_port> <out_host> <out_port> <mod> [mode]
#
# `mode` narrows what is dropped instead: `until-finish` drops every packet
# until the first FINISH goes by (a receiver that joined after the stream
# ended, which must recover every part via RESEND), and `finish` drops every
# FINISH (the receiver holds the whole file but is never told).
#
# The receiver's RESENDs go straight to the sender (not through this proxy), so
# the sender still hears them; its re-sends of the dropped parts pass back through
# here and are dropped again, keeping those parts permanently missing. That lets
# the test layer a junk flood on the receive port and prove the recovery drain
# loop still gives up on its wall-clock deadline instead of spinning forever.

import socket
import struct
import sys

MAGIC = b"FCST"
TYPE_TRANSFER = 2
TYPE_FINISH = 3
HEADER_SIZE = 10  # magic(4) + version(1) + type(1) + session(4); part index at [10:14]


def main(argv):
    if len(argv) < 5:
        print("usage: dropproxy.py <in_port> <out_host> <out_port> <mod> [mode]",
              file=sys.stderr)
        return 2
    in_port, out_host, out_port, mod = int(argv[1]), argv[2], int(argv[3]), int(argv[4])
    mode = argv[5] if len(argv) > 5 else ""
    seen_finish = False

    r = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    r.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    r.bind(("127.0.0.1", in_port))
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    while True:
        data, _ = r.recvfrom(65535)
        ours = len(data) >= HEADER_SIZE and data[:4] == MAGIC
        if mode == "finish" and ours and data[5] == TYPE_FINISH:
            continue
        if mode == "until-finish" and not seen_finish:
            if not (ours and data[5] == TYPE_FINISH):
                continue
            seen_finish = True
            print("first FINISH seen; forwarding from now on", file=sys.stderr, flush=True)
        if not mode and ours and len(data) >= HEADER_SIZE + 4 and data[5] == TYPE_TRANSFER:
            part = struct.unpack(">I", data[HEADER_SIZE:HEADER_SIZE + 4])[0]
            if part % mod == 0:
                continue  # drop this part permanently
        s.sendto(data, (out_host, out_port))


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except KeyboardInterrupt:
        pass  # the test kills us; exit quietly
