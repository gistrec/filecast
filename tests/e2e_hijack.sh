#!/usr/bin/env bash
#
# Session-hijack end-to-end test: a proxy injects one forged ANNOUNCE mid-transfer
# reusing the legitimate session id but claiming a different file_length/sha256/
# name. This pins the same-session ANNOUNCE guard in handleAnnounce: a receiver
# that re-latched it would reset/hijack the transfer, so we require the receiver
# to refuse the forgery, exit 0, report "sha256 verified", produce a byte-identical
# output, write no "evil" file, and log that it ignored the conflicting ANNOUNCE.
#
# FORGE_LENGTH / FORGE_CHUNK select what the forgery claims. The default is a
# well-formed conflicting file; setting FORGE_CHUNK to an out-of-range value (or
# FORGE_LENGTH=0) forges an ANNOUNCE that would *fail* the receiver's range check.
# That case matters because the guard has to drop it before announceValid() runs,
# otherwise the forged packet terminates the receiver -- a one-packet DoS.
#
# Run via:
#   ctest --test-dir build --output-on-failure
#
# Or directly:
#   BINARY=build/filecast bash tests/e2e_hijack.sh
#   BINARY=build/filecast FORGE_CHUNK=1 bash tests/e2e_hijack.sh   # invalid forgery
#
set -euo pipefail

BINARY="${BINARY:-./filecast}"
PYTHON="${PYTHON:-python3}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# What the injected forgery claims. Defaults describe a valid-but-conflicting
# file; override to forge values the receiver's announceValid() would reject.
FORGE_LENGTH="${FORGE_LENGTH:-4096}"
FORGE_CHUNK="${FORGE_CHUNK:-1400}"

if [ ! -x "$BINARY" ]; then
    echo "Error: $BINARY not found or not executable. Build first." >&2
    exit 1
fi

if ! command -v "$PYTHON" >/dev/null 2>&1; then
    echo "Error: $PYTHON not found in PATH; hijack proxy needs Python 3." >&2
    exit 1
fi

WORKDIR="$(mktemp -d -t fb-e2e-hijack.XXXXXX)"
PROXY_PID=""
RECV_PID=""
SEND_PID=""

cleanup() {
    if [ -n "$RECV_PID"  ]; then kill "$RECV_PID"  2>/dev/null || true; fi
    if [ -n "$SEND_PID"  ]; then kill "$SEND_PID"  2>/dev/null || true; fi
    if [ -n "$PROXY_PID" ]; then kill "$PROXY_PID" 2>/dev/null || true; fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

# Topology (all on 127.0.0.1):
#   sender bind   = 33702, sender   target -> 33703 (proxy fwd in, injects forgery)
#   receiver bind = 33701, receiver target -> 33704 (proxy back in, clean)
#   proxy fwd:  33703 -> 33701  (sender->receiver, one forged ANNOUNCE injected)
#   proxy back: 33704 -> 33702  (receiver->sender, untouched)
RECV_BIND=33701
SEND_BIND=33702
PROXY_FWD_IN=33703
PROXY_BACK_IN=33704

dir="$WORKDIR/hijack"
mkdir -p "$dir"
src="$dir/src.bin"
recv_log="$dir/recv.log"
send_log="$dir/send.log"
proxy_log="$dir/proxy.log"

# A few hundred KiB so the transfer spans many parts and is still in progress
# when the forged ANNOUNCE lands (the proxy fires it after the first TRANSFER).
echo "==> generating 500 KiB file (a forged same-session ANNOUNCE will be injected)"
dd if=/dev/urandom of="$src" bs=1024 count=500 status=none

echo "==> starting hijack proxy (forge length=$FORGE_LENGTH chunk=$FORGE_CHUNK)"
"$PYTHON" "$SCRIPT_DIR/hijack_proxy.py" \
    --listen-fwd  "$PROXY_FWD_IN"  --target-fwd  "$RECV_BIND" \
    --listen-back "$PROXY_BACK_IN" --target-back "$SEND_BIND" \
    --forge-length "$FORGE_LENGTH" --forge-chunk "$FORGE_CHUNK" \
    > "$proxy_log" 2>&1 &
PROXY_PID=$!
sleep 0.5  # let the proxy bind both sockets

echo "==> starting receiver"
# An explicit output path keeps every artifact inside $dir and pins the output
# name from the CLI, so the forged ANNOUNCE cannot even influence the file name.
"$BINARY" receive "$dir/out.bin" --to 127.0.0.1 \
          --bind-port "$RECV_BIND" --port "$PROXY_BACK_IN" \
          --ttl 15 --delay-ms 0 > "$recv_log" 2>&1 &
RECV_PID=$!
sleep 1

echo "==> starting sender"
"$BINARY" send "$src" --to 127.0.0.1 \
          --bind-port "$SEND_BIND" --port "$PROXY_FWD_IN" \
          --ttl 10 --delay-ms 0 > "$send_log" 2>&1 &
SEND_PID=$!

# The receiver should ignore the forgery, finish the real transfer and exit 0.
rc=0
wait "$RECV_PID" || rc=$?
RECV_PID=""

kill "$SEND_PID"  2>/dev/null || true; wait "$SEND_PID"  2>/dev/null || true; SEND_PID=""
kill "$PROXY_PID" 2>/dev/null || true; wait "$PROXY_PID" 2>/dev/null || true; PROXY_PID=""

fail() {
    echo "FAIL: $1"
    echo "--- receiver log (tail):"; tail -20 "$recv_log"
    echo "--- proxy log (tail):";    tail -20 "$proxy_log"
    exit 1
}

# Sanity: if the proxy never injected, the test would pass for the wrong reason.
if ! grep -q "injected forged same-session ANNOUNCE" "$proxy_log"; then
    fail "proxy never injected a forged ANNOUNCE"
fi
if [ "$rc" -ne 0 ]; then
    fail "expected receiver exit 0, got $rc (forged ANNOUNCE was honoured?)"
fi
if [ ! -f "$dir/out.bin" ]; then
    fail "receiver did not produce the output file"
fi
if ! cmp -s "$src" "$dir/out.bin"; then
    fail "received file does not match source (transfer was corrupted/reset)"
fi
if ! grep -q "sha256 verified" "$recv_log"; then
    fail "receiver did not report a verified transfer"
fi
# The forged name must never touch the disk.
if [ -f "$dir/evil" ] || [ -f "$dir/evil.part" ]; then
    fail "forged file name was written to disk"
fi
# Proof the guard actually fired rather than the forgery racing past the finish.
if ! grep -q "ignoring conflicting announcement" "$recv_log"; then
    fail "receiver never logged that it refused the conflicting announcement"
fi

echo "PASS: forged same-session ANNOUNCE refused; real transfer verified (exit 0)"
echo
echo "Hijack E2E test passed."
