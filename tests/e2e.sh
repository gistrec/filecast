#!/usr/bin/env bash
#
# End-to-end loopback test: spawn a receiver, then a sender, and verify the
# received bytes are bit-identical to what was sent. Run locally with:
#
#   ctest --test-dir build --output-on-failure
#
# Or directly:
#
#   BINARY=build/filecast bash tests/e2e.sh
#
set -euo pipefail

BINARY="${BINARY:-./filecast}"

if [ ! -x "$BINARY" ]; then
    echo "Error: $BINARY not found or not executable. Run 'make program' first." >&2
    exit 1
fi

WORKDIR="$(mktemp -d -t fb-e2e.XXXXXX)"
trap 'rm -rf "$WORKDIR"' EXIT

run_test() {
    local label="$1"
    local size_kb="$2"
    local recv_port="$3"
    local send_port="$4"
    local recv_ttl="$5"
    local send_ttl="$6"

    local src="$WORKDIR/src-$label.bin"
    local dst="$WORKDIR/dst-$label.bin"
    local recv_log="$WORKDIR/recv-$label.log"
    local send_log="$WORKDIR/send-$label.log"

    echo "==> [$label] generating ${size_kb} KiB file"
    dd if=/dev/urandom of="$src" bs=1024 count="$size_kb" status=none

    # Receiver listens on $recv_port, sends RESEND back to $send_port (sender's bind).
    # --delay-ms 0 removes the inter-packet pause that the protocol uses on real
    # LANs to avoid overrunning receivers; on loopback it just slows tests down.
    echo "==> [$label] starting receiver (bind=$recv_port, target=$send_port)"
    "$BINARY" receive "$dst" --to 127.0.0.1 \
              --bind-port "$recv_port" --port "$send_port" --ttl "$recv_ttl" \
              --delay-ms 0 \
        > "$recv_log" 2>&1 &
    local recv_pid=$!

    # Give the receiver a moment to bind before the sender starts blasting.
    sleep 1

    # Sender listens on $send_port, sends TRANSFER to $recv_port (receiver's bind).
    echo "==> [$label] starting sender (bind=$send_port, target=$recv_port)"
    if ! "$BINARY" send "$src" --to 127.0.0.1 \
                   --bind-port "$send_port" --port "$recv_port" --ttl "$send_ttl" \
                   --delay-ms 0 \
            > "$send_log" 2>&1; then
        echo "FAIL: [$label] sender exited non-zero"
        echo "--- sender log:"; cat "$send_log"
        kill "$recv_pid" 2>/dev/null || true
        return 1
    fi

    # Wait for receiver to drain and exit on its own ttl.
    if ! wait "$recv_pid"; then
        echo "FAIL: [$label] receiver exited non-zero"
        echo "--- receiver log:"; cat "$recv_log"
        return 1
    fi

    if [ ! -f "$dst" ]; then
        echo "FAIL: [$label] receiver did not produce output file"
        echo "--- receiver log:"; cat "$recv_log"
        return 1
    fi

    if ! cmp -s "$src" "$dst"; then
        local src_size dst_size
        src_size=$(wc -c < "$src")
        dst_size=$(wc -c < "$dst")
        echo "FAIL: [$label] received file does not match source"
        echo "       src size: $src_size bytes"
        echo "       dst size: $dst_size bytes"
        echo "--- receiver log (last 20 lines):"
        tail -20 "$recv_log"
        echo "--- sender log (last 20 lines):"
        tail -20 "$send_log"
        return 1
    fi

    echo "PASS: [$label] $size_kb KiB transferred and matches source"
}

# Args: label, size_kb, recv_bind_port, send_bind_port, recv_ttl, send_ttl
#
# Small file: 50 KiB -> ~35 parts at default MTU 1500.
run_test "small" 50    33401 33402 3 1

# Large file: 2 MiB -> ~1400 parts at default MTU 1500.
run_test "large" 2048  33403 33404 3 1

echo
echo "All E2E tests passed."
