#!/usr/bin/env bash
#
# The receiver must not give up while the sender is answering:
#   late-join     everything hidden until the first FINISH, so every part is
#                 recovered via RESEND with a burst longer than --ttl
#   resume-prefix a --resume snapshot holds most parts; the sender streams that
#                 prefix (duplicates) for longer than --ttl
#   lost-finish   every FINISH dropped; the complete file must still verify
# Run: BINARY=build/filecast bash tests/e2e_recovery.sh
set -euo pipefail

BINARY="${BINARY:-./filecast}"
PYTHON="${PYTHON:-python3}"
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -x "$BINARY" ] || { echo "Error: $BINARY not executable. Build first." >&2; exit 1; }

W="$(mktemp -d -t fb-recovery.XXXXXX)"
trap 'pkill -P $$ >/dev/null 2>&1 || true; rm -rf "$W"' EXIT

# Own 342xx port block so ctest -j does not collide with the other e2e tests.
RB=34201; SB=34202; PX=34203; CHUNK=1400
L=""; D=""
die() { echo "FAIL: [$L] $*"; [ -f "$D/recv.log" ] && tail -5 "$D/recv.log"; exit 1; }
proxy() { "$PYTHON" "$HERE/dropproxy.py" "$PX" 127.0.0.1 "$RB" 1 "$1" > "$D/proxy.log" 2>&1 & sleep 0.5; }
recv() { "$BINARY" receive "$D/out.bin" --to 127.0.0.1 --bind-port "$RB" --port "$SB" \
             "$@" > "$D/recv.log" 2>&1 & RP=$!; sleep 1; }
send() { local port="$1"; shift
         "$BINARY" send "$D/src.bin" --to 127.0.0.1 --bind-port "$SB" --port "$port" \
             --mtu "$CHUNK" "$@" > "$D/send.log" 2>&1 & }
reap() { rc=0; wait "$RP" || rc=$?; pkill -P $$ >/dev/null 2>&1 || true; }
verified() {
    [ "$rc" -eq 0 ] || die "expected exit 0, got $rc"
    cmp -s "$D/src.bin" "$D/out.bin" || die "received file does not match source"
    grep -q "sha256 verified" "$D/recv.log" || die "receiver did not verify the transfer"
    [ ! -e "$D/out.bin.part" ] || die "out.bin.part left behind"
}

L="late-join"; D="$W/$L"; mkdir -p "$D"
echo "==> [$L] every part recovered via RESEND with a burst longer than --ttl"
# ~1500 parts; --delay-ms 10 paces the RESEND burst to ~15 s, 3x the --ttl.
dd if=/dev/urandom of="$D/src.bin" bs=1024 count=2048 status=none
proxy until-finish
recv --ttl 5 --delay-ms 10
send "$PX" --ttl 10 --delay-ms 0
reap
grep -q "first FINISH seen" "$D/proxy.log" || die "proxy never hid the stream"
verified
echo "PASS: [$L] full recovery past the old blind-burst deadline"

L="resume-prefix"; D="$W/$L"; mkdir -p "$D"
echo "==> [$L] duplicates must keep a resumed receiver alive"
dd if=/dev/urandom of="$D/src.bin" bs=1024 count=300 status=none
# Snapshot holding all but the last 20 parts; .part.idx as the receiver writes
# it: "FCIDX1"+NUL(7) + sha256(32) + length(8) + chunk(4) + bitmap.
"$PYTHON" - "$D/src.bin" "$D/out.bin.part" "$D/out.bin.part.idx" "$CHUNK" <<'PY'
import hashlib, sys
src, part, idx, chunk = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
data = open(src, "rb").read()
total = (len(data) + chunk - 1) // chunk
keep = total - 20
with open(part, "wb") as f:
    f.write(data[:keep * chunk]); f.truncate(len(data))
bitmap = bytearray((total + 7) // 8)
for p in range(keep):
    bitmap[p // 8] |= 1 << (p % 8)
with open(idx, "wb") as f:
    f.write(b"FCIDX1\0" + hashlib.sha256(data).digest()
            + len(data).to_bytes(8, "big") + chunk.to_bytes(4, "big") + bytes(bitmap))
print(f"snapshot: {keep}/{total} parts present")
PY
# ~5 s of duplicates (200 held parts at 25 ms), past --ttl 3.
recv --ttl 3 --delay-ms 0 --resume
send "$RB" --ttl 10 --delay-ms 25
reap
grep -q "Resuming .*parts already present" "$D/recv.log" || die "receiver did not resume"
verified
[ ! -e "$D/out.bin.part.idx" ] || die "out.bin.part.idx left behind"
echo "PASS: [$L] duplicates kept the deadline alive; transfer verified"

L="lost-finish"; D="$W/$L"; mkdir -p "$D"
echo "==> [$L] a complete file must verify even with every FINISH lost"
dd if=/dev/urandom of="$D/src.bin" bs=1024 count=300 status=none
proxy finish
recv --ttl 4 --delay-ms 0
send "$PX" --ttl 3 --delay-ms 0
reap
verified
echo "PASS: [$L] transfer completed without a FINISH"

echo
echo "Recovery E2E test passed."
