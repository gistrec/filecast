#!/usr/bin/env bash
#
# Pre-latch ANNOUNCE DoS regression test. Before a receiver has latched a session,
# a forged ANNOUNCE carrying an invalid file_size/chunk must be IGNORED (the
# receiver keeps waiting), not terminate it. Previously such a packet drove the
# receiver straight to exit 2 ("Sender announced empty file"), so a single
# unauthenticated datagram killed `filecast receive` in the very window it spends
# "Waiting for a sender..." — a one-packet DoS. The same-session guard already
# covered the post-latch case (see e2e_hijack.sh); this pins the pre-latch one.
#
# The test fires two poison ANNOUNCEs (empty file, out-of-range chunk) at the
# receiver's port BEFORE the real sender starts, checks the receiver is still
# alive, then runs a normal transfer and requires it to complete and verify —
# proving the poison was ignored, not fatal.
#
# Run via:
#   ctest --test-dir build --output-on-failure
#
# Or directly:
#   BINARY=build/filecast bash tests/e2e_prelatch.sh
#
set -euo pipefail

BINARY="${BINARY:-./filecast}"
PYTHON="${PYTHON:-python3}"

if [ ! -x "$BINARY" ]; then
    echo "Error: $BINARY not found or not executable. Build first." >&2
    exit 1
fi
if ! command -v "$PYTHON" >/dev/null 2>&1; then
    echo "Error: $PYTHON not found in PATH; the poison sender needs Python 3." >&2
    exit 1
fi

WORKDIR="$(mktemp -d -t fb-e2e-prelatch.XXXXXX)"
RECV_PID=""
SEND_PID=""
cleanup() {
    if [ -n "$RECV_PID" ]; then kill "$RECV_PID" 2>/dev/null || true; fi
    if [ -n "$SEND_PID" ]; then kill "$SEND_PID" 2>/dev/null || true; fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

# Own 338xx port block so ctest -j doesn't collide with the other e2e tests.
RECV_BIND=33801
SEND_BIND=33802

# Fire one forged ANNOUNCE (reusing a fixed session id, claiming file_size/chunk)
# straight at the receiver's port.
send_poison() {
    local port="$1" file_size="$2" chunk="$3"
    "$PYTHON" - "$port" "$file_size" "$chunk" <<'PY'
import socket, struct, sys
port, file_size, chunk = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
# Wire layout from src/Protocol.hpp (v3): magic"FCST"(4) + version(1) + type(1) +
# session(4) + file_size(4) + chunk_size(4) + sha256(32) + name_len(2) + name.
pkt  = b"FCST" + struct.pack("!BB", 3, 1) + struct.pack("!I", 0x11223344)
pkt += struct.pack("!I", file_size) + struct.pack("!I", chunk) + b"\x00" * 32
name = b"evil"
pkt += struct.pack("!H", len(name)) + name
socket.socket(socket.AF_INET, socket.SOCK_DGRAM).sendto(pkt, ("127.0.0.1", port))
PY
}

src="$WORKDIR/src.bin"
dst="$WORKDIR/out.bin"
recv_log="$WORKDIR/recv.log"
send_log="$WORKDIR/send.log"

echo "==> generating 200 KiB file"
dd if=/dev/urandom of="$src" bs=1024 count=200 status=none

echo "==> starting receiver (bind=$RECV_BIND)"
"$BINARY" receive "$dst" --to 127.0.0.1 \
          --bind-port "$RECV_BIND" --port "$SEND_BIND" --ttl 10 --delay-ms 0 \
    > "$recv_log" 2>&1 &
RECV_PID=$!
sleep 1  # let it bind and enter the pre-latch "Waiting for a sender..." state

echo "==> firing two poison ANNOUNCEs (empty file, out-of-range chunk) pre-latch"
send_poison "$RECV_BIND" 0    1500   # empty file      -> must be ignored, not fatal
send_poison "$RECV_BIND" 4096 1      # out-of-range chunk -> must be ignored, not fatal
sleep 0.5  # give the receiver time to process them (and, if buggy, to die)

# If the receiver had died on the poison it would be gone now, and the transfer
# below would never complete — catch it here for a crisp failure message.
if ! kill -0 "$RECV_PID" 2>/dev/null; then
    echo "FAIL: receiver exited on a poison ANNOUNCE (pre-latch DoS regression)"
    tail -20 "$recv_log"
    exit 1
fi

echo "==> starting real sender (bind=$SEND_BIND)"
"$BINARY" send "$src" --to 127.0.0.1 \
          --bind-port "$SEND_BIND" --port "$RECV_BIND" --ttl 3 --delay-ms 0 \
    > "$send_log" 2>&1 &
SEND_PID=$!

rc=0
wait "$RECV_PID" || rc=$?
RECV_PID=""
kill "$SEND_PID" 2>/dev/null || true; wait "$SEND_PID" 2>/dev/null || true; SEND_PID=""

if [ "$rc" -ne 0 ]; then
    echo "FAIL: expected receiver exit 0 after ignoring poison, got $rc"
    tail -20 "$recv_log"
    exit 1
fi
if [ ! -f "$dst" ] || ! cmp -s "$src" "$dst"; then
    echo "FAIL: received file missing or does not match source"
    tail -20 "$recv_log"
    exit 1
fi
if ! grep -q "sha256 verified" "$recv_log"; then
    echo "FAIL: receiver did not verify the transfer"
    tail -20 "$recv_log"
    exit 1
fi
# Proof the poison was actually seen and ignored, not just missed by timing.
if ! grep -q "ignoring announcement with invalid" "$recv_log"; then
    echo "FAIL: receiver never logged that it ignored the poison ANNOUNCE"
    tail -20 "$recv_log"
    exit 1
fi

echo "PASS: pre-latch poison ANNOUNCEs ignored; real transfer completed and verified"
echo
echo "Pre-latch E2E test passed."
