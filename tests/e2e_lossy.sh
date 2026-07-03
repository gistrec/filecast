#!/usr/bin/env bash
#
# Lossy end-to-end test: spawn a UDP proxy that drops packets at a configurable
# rate in both directions, run sender + receiver through it, and verify that
# the RESEND branch recovers the file bit-identically.
#
# Run via:
#   ctest --test-dir build --output-on-failure
#
# Or directly:
#   BINARY=build/filecast bash tests/e2e_lossy.sh
#
set -euo pipefail

BINARY="${BINARY:-./filecast}"
PYTHON="${PYTHON:-python3}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ ! -x "$BINARY" ]; then
    echo "Error: $BINARY not found or not executable. Build first." >&2
    exit 1
fi

if ! command -v "$PYTHON" >/dev/null 2>&1; then
    echo "Error: $PYTHON not found in PATH; lossy proxy needs Python 3." >&2
    exit 1
fi

WORKDIR="$(mktemp -d -t fb-e2e-lossy.XXXXXX)"
PROXY_PID=""
RECV_PID=""

cleanup() {
    [ -n "$RECV_PID"  ] && kill "$RECV_PID"  2>/dev/null || true
    [ -n "$PROXY_PID" ] && kill "$PROXY_PID" 2>/dev/null || true
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

# Topology (all on 127.0.0.1):
#   sender bind   = 33502, sender   target -> 33503 (proxy fwd in)
#   receiver bind = 33501, receiver target -> 33504 (proxy back in)
#   proxy fwd:  33503 -> 33501  (drops in sender->receiver direction)
#   proxy back: 33504 -> 33502  (drops in receiver->sender direction)
RECV_BIND=33501
SEND_BIND=33502
PROXY_FWD_IN=33503
PROXY_BACK_IN=33504

run_test() {
    local label="$1"
    local size_kb="$2"
    local drop_rate="$3"

    local src="$WORKDIR/src-$label.bin"
    local dst="$WORKDIR/dst-$label.bin"
    local recv_log="$WORKDIR/recv-$label.log"
    local send_log="$WORKDIR/send-$label.log"
    local proxy_log="$WORKDIR/proxy-$label.log"

    echo "==> [$label] generating ${size_kb} KiB file (drop rate ${drop_rate} each way)"
    dd if=/dev/urandom of="$src" bs=1024 count="$size_kb" status=none

    echo "==> [$label] starting lossy proxy"
    "$PYTHON" "$SCRIPT_DIR/lossy_proxy.py" \
        --listen-fwd  "$PROXY_FWD_IN"  --target-fwd  "$RECV_BIND" \
        --listen-back "$PROXY_BACK_IN" --target-back "$SEND_BIND" \
        --drop-rate "$drop_rate" --seed 1 \
        > "$proxy_log" 2>&1 &
    PROXY_PID=$!
    # Give the proxy a moment to bind both sockets.
    sleep 0.5

    echo "==> [$label] starting receiver (bind=$RECV_BIND, target=$PROXY_BACK_IN)"
    "$BINARY" receive "$dst" --to 127.0.0.1 \
              --bind-port "$RECV_BIND" --port "$PROXY_BACK_IN" \
              --ttl 15 --delay-ms 0 \
        > "$recv_log" 2>&1 &
    RECV_PID=$!
    sleep 1

    echo "==> [$label] starting sender (bind=$SEND_BIND, target=$PROXY_FWD_IN)"
    if ! "$BINARY" send "$src" --to 127.0.0.1 \
                   --bind-port "$SEND_BIND" --port "$PROXY_FWD_IN" \
                   --ttl 10 --delay-ms 0 \
            > "$send_log" 2>&1; then
        echo "FAIL: [$label] sender exited non-zero"
        echo "--- sender log (tail):";  tail -30 "$send_log"
        echo "--- proxy log (tail):";   tail -30 "$proxy_log"
        return 1
    fi

    if ! wait "$RECV_PID"; then
        echo "FAIL: [$label] receiver exited non-zero"
        echo "--- receiver log (tail):"; tail -30 "$recv_log"
        echo "--- proxy log (tail):";    tail -30 "$proxy_log"
        return 1
    fi
    RECV_PID=""

    kill "$PROXY_PID" 2>/dev/null || true
    wait "$PROXY_PID" 2>/dev/null || true
    PROXY_PID=""

    if [ ! -f "$dst" ]; then
        echo "FAIL: [$label] receiver did not produce output file"
        echo "--- receiver log (tail):"; tail -30 "$recv_log"
        return 1
    fi

    if ! cmp -s "$src" "$dst"; then
        local src_size dst_size
        src_size=$(wc -c < "$src")
        dst_size=$(wc -c < "$dst")
        echo "FAIL: [$label] received file does not match source"
        echo "       src size: $src_size, dst size: $dst_size"
        echo "--- receiver log (tail):"; tail -30 "$recv_log"
        echo "--- sender log (tail):";   tail -30 "$send_log"
        echo "--- proxy log (tail):";    tail -30 "$proxy_log"
        return 1
    fi

    # Confirm we actually used RESEND. If not, the proxy's drop rate was too
    # low to lose a single packet on this seed and the test is misconfigured.
    local resend_count
    resend_count=$(grep -c "Request part of file with index" "$recv_log" || true)
    if [ "$resend_count" -eq 0 ]; then
        echo "FAIL: [$label] file matched but RESEND branch was never exercised"
        echo "       drop rate ${drop_rate} produced no losses with seed 1"
        return 1
    fi
    echo "PASS: [$label] ${size_kb} KiB recovered through ${resend_count} RESENDs"
}

# Args: label, size_kb, drop_rate
run_test "lossy-light"    50 0.10
run_test "lossy-moderate" 50 0.30

echo
echo "All lossy E2E tests passed."
