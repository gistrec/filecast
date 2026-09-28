#!/usr/bin/env bash
#
# The receiver must never truncate or delete a <name>.part / <name>.part.idx it
# did not create, whatever an unauthenticated ANNOUNCE claims, while still
# adopting its own --resume snapshot and replacing a stale pair under
# --overwrite. Run: BINARY=build/filecast bash tests/e2e_partguard.sh
set -euo pipefail

BINARY="${BINARY:-./filecast}"
PYTHON="${PYTHON:-python3}"
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -x "$BINARY" ] || { echo "Error: $BINARY not executable. Build first." >&2; exit 1; }

W="$(mktemp -d -t fb-partguard.XXXXXX)"
trap 'pkill -P $$ >/dev/null 2>&1 || true; rm -rf "$W"' EXIT

# Own 341xx port block so ctest -j does not collide with the other e2e tests.
RB=34101; SB=34102
L=""; D=""
die() { echo "FAIL: [$L] $*"; [ -f "$D/recv.log" ] && tail -5 "$D/recv.log"; exit 1; }
# The injector talks straight to the receiver's bind port; no proxy involved.
inject() { "$PYTHON" "$HERE/inject_announce.py" "$RB" 3 >/dev/null 2>&1; }
recv() { "$BINARY" receive "$D/out.bin" --to 127.0.0.1 --bind-port "$RB" --port "$SB" \
             --delay-ms 0 "$@" > "$D/recv.log" 2>&1 & RP=$!; sleep 1; }
send() { "$BINARY" send "$D/src.bin" --to 127.0.0.1 --bind-port "$SB" --port "$RB" \
             --ttl 10 --delay-ms 0 --mtu 1400 > "$D/send.log" 2>&1 & SP=$!; }
# Reap the receiver, then the sender quietly (an unwaited job prints a
# "Terminated" notification of its own).
reap() {
    rc=0; wait "$RP" || rc=$?
    if [ -n "${SP:-}" ]; then kill "$SP" 2>/dev/null || true; wait "$SP" 2>/dev/null || true; SP=""; fi
}

# Three decoys at our names that the receiver did not create. Each must survive
# a burst of foreign ANNOUNCEs byte-for-byte (the directory pins that cleanup
# never rmdir's), and the announcement must be refused with a warning.
for decoy in file dir orphan-idx; do
    L="decoy-$decoy"; D="$W/$L"; mkdir -p "$D"
    case "$decoy" in
        file)       printf 'not ours\n' > "$D/out.bin.part";     V="$D/out.bin.part" ;;
        dir)        mkdir "$D/out.bin.part"; : > "$D/out.bin.part/keep"; V="$D/out.bin.part" ;;
        orphan-idx) printf 'not ours\n' > "$D/out.bin.part.idx"; V="$D/out.bin.part.idx" ;;
    esac
    [ -d "$V" ] || cp "$V" "$D/orig"

    echo "==> [$L] foreign ANNOUNCEs against $(basename "$V")"
    recv --ttl 3 --resume
    inject
    reap

    [ "$rc" -eq 2 ] || die "expected timeout exit 2, got $rc"
    if [ -d "$V" ]; then
        [ -f "$V/keep" ] || die "the directory at out.bin.part was removed"
    else
        cmp -s "$V" "$D/orig" || die "$(basename "$V") was modified or deleted"
        [ ! -e "$D/out.bin" ] || die "an output file appeared out of nowhere"
    fi
    grep -q "already exists and is not from this transfer" "$D/recv.log" \
        || die "receiver never warned that it refused the announcement"
    echo "PASS: [$L] left untouched, announcement refused"
done

L="overwrite"; D="$W/$L"; mkdir -p "$D"
echo "==> [$L] --overwrite replaces a stale pair and the transfer verifies"
dd if=/dev/urandom of="$D/src.bin" bs=1024 count=300 status=none
printf 'stale part\n' > "$D/out.bin.part"; printf 'stale idx\n' > "$D/out.bin.part.idx"
recv --ttl 15 --overwrite
send
reap
[ "$rc" -eq 0 ] || die "expected exit 0, got $rc"
cmp -s "$D/src.bin" "$D/out.bin" || die "received file does not match source"
[ ! -e "$D/out.bin.part" ] && [ ! -e "$D/out.bin.part.idx" ] || die "stale files left behind"
echo "PASS: [$L] stale pair replaced, transfer verified"

L="resume"; D="$W/$L"; mkdir -p "$D"
echo "==> [$L] foreign ANNOUNCEs must not destroy a genuine --resume snapshot"
dd if=/dev/urandom of="$D/src.bin" bs=1024 count=300 status=none
# A half-finished snapshot exactly as the receiver leaves it: half the parts in
# out.bin.part, and .part.idx = "FCIDX1"+NUL(7)+sha256(32)+len(8)+chunk(4)+bitmap.
"$PYTHON" - "$D/src.bin" "$D/out.bin.part" "$D/out.bin.part.idx" 1400 <<'PY'
import hashlib, sys
src, part, idx, chunk = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
data = open(src, "rb").read()
total = (len(data) + chunk - 1) // chunk
keep = total // 2
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
cp "$D/out.bin.part" "$D/part.orig"; cp "$D/out.bin.part.idx" "$D/idx.orig"

recv --ttl 15 --resume
inject
sleep 1
cmp -s "$D/out.bin.part" "$D/part.orig" || die "foreign ANNOUNCE modified the snapshot's .part"
cmp -s "$D/out.bin.part.idx" "$D/idx.orig" || die "foreign ANNOUNCE modified the snapshot's .idx"
send
reap
[ "$rc" -eq 0 ] || die "expected exit 0, got $rc"
cmp -s "$D/src.bin" "$D/out.bin" || die "received file does not match source"
grep -q "Resuming .*parts already present" "$D/recv.log" || die "receiver did not resume"
[ ! -e "$D/out.bin.part" ] && [ ! -e "$D/out.bin.part.idx" ] || die "snapshot left behind"
echo "PASS: [$L] snapshot survived foreign announcements and was resumed"

echo
echo "Part-guard E2E test passed."
