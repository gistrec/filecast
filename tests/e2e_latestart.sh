#!/usr/bin/env bash
#
# A receiver that starts seconds into the stream must latch from a mid-stream
# ANNOUNCE repeat and complete, instead of only ever seeing un-announced data
# and timing out while the sender is still broadcasting.
# Run: BINARY=build/filecast bash tests/e2e_latestart.sh
set -euo pipefail

BINARY="${BINARY:-./filecast}"
[ -x "$BINARY" ] || { echo "Error: $BINARY not executable. Build first." >&2; exit 1; }

W="$(mktemp -d -t fb-latestart.XXXXXX)"
trap 'pkill -P $$ >/dev/null 2>&1 || true; rm -rf "$W"' EXIT

# Own 343xx port block so ctest -j does not collide with the other e2e tests.
RB=34301; SB=34302
die() { echo "FAIL: [late-start] $*"; tail -5 "$W/recv.log"; exit 1; }

# ~750 parts paced 10 ms apart (~7.5 s). The receiver starts 2.5 s in with
# --ttl 3: the initial burst and first-second repeats are long gone, and the
# resend-phase repeats come too late.
dd if=/dev/urandom of="$W/src.bin" bs=1024 count=1024 status=none

echo "==> [late-start] receiver starts 2.5 s into the stream"
"$BINARY" send "$W/src.bin" --to 127.0.0.1 --bind-port "$SB" --port "$RB" \
          --ttl 10 --delay-ms 10 --mtu 1400 > "$W/send.log" 2>&1 &
sleep 2.5
"$BINARY" receive "$W/out.bin" --to 127.0.0.1 --bind-port "$RB" --port "$SB" \
          --ttl 3 --delay-ms 0 > "$W/recv.log" 2>&1 &
rc=0; wait $! || rc=$?
pkill -P $$ >/dev/null 2>&1 || true

# Sanity: it really did start mid-stream (data seen before any announcement).
grep -q "receiving data without an announcement" "$W/recv.log" \
    || die "receiver never saw un-announced data; it did not start mid-stream"
[ "$rc" -eq 0 ] || die "expected exit 0, got $rc (never latched the running transfer?)"
cmp -s "$W/src.bin" "$W/out.bin" || die "received file does not match source"
grep -q "sha256 verified" "$W/recv.log" || die "receiver did not verify the transfer"

echo "PASS: [late-start] receiver latched a running transfer and completed it"
