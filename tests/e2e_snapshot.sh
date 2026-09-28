#!/usr/bin/env bash
#
# What the receiver keeps, deletes and says when a transfer does not complete:
#   idx-fail   a directory at <name>.part.idx blocks the index write, so the
#              flushed <name>.part must stay and the message must say why
#   empty-snap every data packet dropped: a --resume timeout with zero parts
#              must leave no preallocated .part and no all-zero index
#   silent     a receive that never hears a sender must say why it exits 2
#   long-name  a 246-byte name (MAX_NAME_LEN) must still get its index
# Run: BINARY=build/filecast bash tests/e2e_snapshot.sh
set -euo pipefail

BINARY="${BINARY:-./filecast}"
PYTHON="${PYTHON:-python3}"
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -x "$BINARY" ] || { echo "Error: $BINARY not executable. Build first." >&2; exit 1; }

W="$(mktemp -d -t fb-snapshot.XXXXXX)"
trap 'pkill -P $$ >/dev/null 2>&1 || true; rm -rf "$W"' EXIT

# Own 344xx port block so ctest -j does not collide with the other e2e tests.
# The last scenario asserts nothing arrives, so it gets its own pair.
RB=34401; SB=34402; PX=34403; IB=34404; IP=34405
L=""; D=""; RL=""
die() { echo "FAIL: [$L] $*"; [ -f "$RL" ] && tail -5 "$RL"; exit 1; }
send() { "$BINARY" send "$1" --to 127.0.0.1 --bind-port "$SB" --port "$2" \
             --ttl 5 --delay-ms "${3:-0}" --mtu 1400 > "$D/send.log" 2>&1 & }
reap() { rc=0; wait "$RP" || rc=$?; pkill -P $$ >/dev/null 2>&1 || true; }

L="idx-fail"; D="$W/$L"; mkdir -p "$D"; RL="$D/recv.log"
echo "==> [$L] an unwritable index must not cost the received bytes"
dd if=/dev/urandom of="$D/src.bin" bs=1024 count=1024 status=none
# A directory cannot be renamed over: the index write fails with the .part
# already flushed, which is the split this pins.
mkdir "$D/out.bin.part.idx"
"$BINARY" receive "$D/out.bin" --to 127.0.0.1 --bind-port "$RB" --port "$SB" \
          --ttl 10 --delay-ms 0 --resume > "$RL" 2>&1 & RP=$!
sleep 1; send "$D/src.bin" "$RB" 5; sleep 2
kill -INT "$RP" 2>/dev/null || true
reap
[ "$rc" -eq 130 ] || die "expected interrupt exit 130, got $rc"
[ -s "$D/out.bin.part" ] || die "the flushed .part was deleted because the index failed"
[ -d "$D/out.bin.part.idx" ] || die "the planted directory was removed"
grep -q "could not write .*out.bin.part.idx" "$RL" || die "the index failure was not reported"
grep -q "received data kept in .*out.bin.part" "$RL" || die "did not say where the data was kept"
echo "PASS: [$L] .part kept and the index failure reported"

L="empty-snap"; D="$W/$L"; mkdir -p "$D"; RL="$D/recv.log"
echo "==> [$L] a --resume timeout with zero parts must leave nothing behind"
dd if=/dev/urandom of="$D/src.bin" bs=1024 count=4096 status=none
# mod 1 drops every TRANSFER; ANNOUNCE and FINISH get through, so the receiver
# latches, learns the 4 MiB size and times out without a single part.
"$PYTHON" "$HERE/dropproxy.py" "$PX" 127.0.0.1 "$RB" 1 > "$D/proxy.log" 2>&1 &
sleep 0.5
"$BINARY" receive "$D/out.bin" --to 127.0.0.1 --bind-port "$RB" --port "$SB" \
          --ttl 3 --delay-ms 0 --resume > "$RL" 2>&1 & RP=$!
sleep 1; send "$D/src.bin" "$PX"
reap
[ "$rc" -eq 2 ] || die "expected timeout exit 2, got $rc"
[ ! -e "$D/out.bin.part" ] || die "left a $(wc -c < "$D/out.bin.part")-byte .part behind"
[ ! -e "$D/out.bin.part.idx" ] || die "left an index behind"
grep -q "part(s) missing" "$RL" || die "the missing parts were not reported"
! grep -q "progress saved" "$RL" || die "claimed progress was saved with zero parts"
echo "PASS: [$L] nothing kept when no part ever arrived"

L="silent"; D="$W/$L"; mkdir -p "$D"; RL="$D/recv.log"
echo "==> [$L] a receive that never hears a sender must say why it failed"
"$BINARY" receive "$D/out.bin" --to 127.0.0.1 --bind-port "$IB" --port "$IP" \
          --ttl 2 --delay-ms 0 > "$RL" 2>&1 & RP=$!
reap
[ "$rc" -eq 2 ] || die "expected timeout exit 2, got $rc"
grep -q "timed out" "$RL" || die "exited 2 without saying why"
echo "PASS: [$L] timeout reported instead of a silent exit 2"

L="long-name"; D="$W/$L"; mkdir -p "$D/src" "$D/out"; RL="$D/out/recv.log"
echo "==> [$L] a maximum-length name must still get its snapshot index"
# 246 bytes is Protocol::MAX_NAME_LEN: "<name>.part.idx" is exactly NAME_MAX, so
# the index write has no room for a longer temp name beside it.
name="$(printf 'L%.0s' $(seq 1 246))"
dd if=/dev/urandom of="$D/src/$name" bs=1024 count=4096 status=none
( cd "$D/out" && exec "$BINARY" receive --to 127.0.0.1 --bind-port "$RB" --port "$SB" \
      --ttl 8 --delay-ms 0 --mtu 1400 --resume > recv.log 2>&1 ) & RP=$!
sleep 1; send "$D/src/$name" "$RB" 5; sleep 3
pkill -INT -f "receive --to 127.0.0.1 --bind-port $RB" 2>/dev/null || true
reap
[ "$rc" -eq 130 ] || die "expected interrupt exit 130, got $rc"
[ -s "$D/out/$name.part.idx" ] || die "no snapshot index written for a ${#name}-byte name"
grep -q "progress saved" "$RL" || die "no saved snapshot reported"
echo "PASS: [$L] snapshot index written beside a ${#name}-byte name"

echo
echo "Snapshot E2E test passed."
