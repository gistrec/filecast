#!/usr/bin/env python3
#
# UDP flooder for the timeout-bound E2E tests (tests/e2e_flood.sh). It keeps a
# target socket continuously busy so recvfrom never reports an idle read, which
# is exactly the condition that used to keep filecast's receive/resend loops
# alive forever. Two modes:
#
#   garbage <host> <port> <duration>
#       Blast tiny junk datagrams at host:port. Each is too short to be one of
#       our packets (parseHeader -> NotOurs), so a correct receiver ignores them
#       yet must still time out on its wall-clock deadline.
#
#   resend <sniff_port> <target_host> <target_port> <duration>
#       Listen on sniff_port for the sender's broadcasts to learn its (cleartext)
#       session id, then flood well-formed RESEND packets for part 0 at
#       target_host:target_port. A correct sender serves them rate-limited but
#       still stops at its absolute phase deadline instead of running forever.
#
# The flooder exits on its own after <duration> seconds as a backstop, but the
# test normally kills it once the process under test has exited.

import socket
import struct
import sys
import time

MAGIC = b"FCST"
VERSION = 3
TYPE_RESEND = 4
HEADER_SIZE = 10  # magic(4) + version(1) + type(1) + session(4)

# Burst size between clock checks: large enough to keep the socket buffer full on
# loopback, small enough that the duration backstop stays responsive.
BURST = 256


def make_resend(session, part):
    # header + part(4), all big-endian, matching Protocol::writeHeader + putU32.
    return MAGIC + bytes([VERSION, TYPE_RESEND]) + struct.pack(">II", session, part)


def garbage(host, port, duration):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    deadline = time.time() + duration
    while time.time() < deadline:
        for _ in range(BURST):
            try:
                s.sendto(b"x", (host, port))
            except OSError:
                pass  # transient ENOBUFS/EAGAIN under a heavy loopback flood


def resend(sniff_port, target_host, target_port, duration):
    r = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    r.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    r.bind(("", sniff_port))
    r.settimeout(0.05)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    session = None
    deadline = time.time() + duration
    while time.time() < deadline:
        # Keep draining the sniff socket so a fresh sender run re-latches the id.
        try:
            data, _ = r.recvfrom(65535)
            if len(data) >= HEADER_SIZE and data[:4] == MAGIC:
                session = struct.unpack(">I", data[6:10])[0]
        except socket.timeout:
            pass
        except OSError:
            pass
        if session is None:
            continue
        pkt = make_resend(session, 0)
        for _ in range(BURST):
            try:
                s.sendto(pkt, (target_host, target_port))
            except OSError:
                pass


def main(argv):
    if len(argv) < 2:
        print("usage: flood.py garbage|resend ...", file=sys.stderr)
        return 2
    mode = argv[1]
    try:
        if mode == "garbage":
            garbage(argv[2], int(argv[3]), float(argv[4]))
        elif mode == "resend":
            resend(int(argv[2]), argv[3], int(argv[4]), float(argv[5]))
        else:
            print("unknown mode: " + mode, file=sys.stderr)
            return 2
    except KeyboardInterrupt:
        pass  # the test kills us; exit quietly
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
