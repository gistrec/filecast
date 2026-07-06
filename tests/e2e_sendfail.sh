#!/usr/bin/env bash
#
# Send-failure end-to-end test: run a sender, but preload a shim
# (tests/failsendto) that forces sendto() to fail with ENETDOWN — exactly what a
# downed interface / unreachable network does locally. A total local send
# failure is distinguishable from ordinary UDP loss (the packet never reaches
# the kernel), so the sender must report it and exit non-zero rather than print
# "Sent ..." and return 0, which would let `filecast send f && next` proceed as
# if the file had gone out.
#
# Two branches are covered:
#   * FAILSENDTO_TYPE=0 fails every packet  -> all ANNOUNCEs fail (sendAnnounce)
#   * FAILSENDTO_TYPE=2 fails only TRANSFER -> ANNOUNCE goes out but no part does
#
# No receiver is involved: the fault is injected locally in the sender.
#
# Run via:
#   ctest --test-dir build --output-on-failure
#
# Or directly:
#   BINARY=build/filecast FAILLIB=build/libfailsendto.so bash tests/e2e_sendfail.sh
#
set -euo pipefail

BINARY="${BINARY:-./filecast}"
FAILLIB="${FAILLIB:-}"

if [ ! -x "$BINARY" ]; then
    echo "Error: $BINARY not found or not executable. Build first." >&2
    exit 1
fi
if [ -z "$FAILLIB" ] || [ ! -f "$FAILLIB" ]; then
    echo "Error: FAILLIB ($FAILLIB) not found; build the failsendto shim first." >&2
    exit 1
fi

case "$(uname -s)" in
    Darwin) PRELOAD_VAR="DYLD_INSERT_LIBRARIES" ;;
    *)      PRELOAD_VAR="LD_PRELOAD" ;;
esac

# Waive only ASan's "runtime does not come first" guard for the preloaded shim
# (see tests/e2e_diskfull.sh); every other ASan check stays active.
SEND_ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}verify_asan_link_order=0"

WORKDIR="$(mktemp -d -t fb-e2e-sendfail.XXXXXX)"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

SRC="$WORKDIR/src.bin"
dd if=/dev/urandom of="$SRC" bs=1024 count=20 status=none

# Args: label, FAILSENDTO_TYPE value, expected stderr substring
run_sendfail_test() {
    local label="$1"
    local fail_type="$2"
    local expect="$3"
    local log="$WORKDIR/$label.log"

    echo "==> [$label] sending with sendto->ENETDOWN (FAILSENDTO_TYPE=$fail_type)"
    local rc=0
    env "$PRELOAD_VAR=$FAILLIB" ASAN_OPTIONS="$SEND_ASAN_OPTIONS" FAILSENDTO_TYPE="$fail_type" \
        "$BINARY" send "$SRC" --to 127.0.0.1 \
                  --bind-port 33712 --port 33711 \
                  --ttl 5 --delay-ms 0 > "$log" 2>&1 || rc=$?

    if [ "$rc" -eq 0 ]; then
        echo "FAIL: [$label] sender exited 0 despite every send failing"
        echo "--- sender log (tail):"; tail -20 "$log"
        return 1
    fi
    if ! grep -q "$expect" "$log"; then
        echo "FAIL: [$label] sender did not report the send failure (expected '$expect')"
        echo "--- sender log (tail):"; tail -20 "$log"
        return 1
    fi
    echo "PASS: [$label] send failure reported (exit $rc, '$expect')"
}

# Every packet fails: the ANNOUNCE never leaves the host, so no receiver can start.
run_sendfail_test "sendfail-announce"  0 "Failed to send ANNOUNCE"
# ANNOUNCE goes out but every data part is rejected by the kernel.
run_sendfail_test "sendfail-transfer"  2 "Failed to send any data"

echo
echo "All send-failure E2E tests passed."
