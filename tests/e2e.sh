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

    # Destination flags: unicast loopback by default; overridable (e.g. multicast).
    # Split into an array so a multi-word DEST expands into separate arguments.
    local dest
    read -ra dest <<< "${DEST:---to 127.0.0.1}"

    echo "==> [$label] generating ${size_kb} KiB file"
    dd if=/dev/urandom of="$src" bs=1024 count="$size_kb" status=none

    # Receiver listens on $recv_port, sends RESEND back to $send_port (sender's bind).
    # --delay-ms 0 removes the inter-packet pause that the protocol uses on real
    # LANs to avoid overrunning receivers; on loopback it just slows tests down.
    echo "==> [$label] starting receiver (bind=$recv_port, target=$send_port)"
    "$BINARY" receive "$dst" "${dest[@]}" \
              --bind-port "$recv_port" --port "$send_port" --ttl "$recv_ttl" \
              --delay-ms 0 \
        > "$recv_log" 2>&1 &
    local recv_pid=$!

    # Give the receiver a moment to bind before the sender starts blasting.
    sleep 1

    # Sender listens on $send_port, sends TRANSFER to $recv_port (receiver's bind).
    echo "==> [$label] starting sender (bind=$send_port, target=$recv_port)"
    if ! "$BINARY" send "$src" "${dest[@]}" \
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

    # The receiver must have verified the SHA-256 the sender announced.
    if ! grep -q "sha256 verified" "$recv_log"; then
        echo "FAIL: [$label] receiver did not verify the file checksum"
        tail -20 "$recv_log"
        return 1
    fi

    echo "PASS: [$label] $size_kb KiB transferred, checksum verified"
}

# Args: label, size_kb, recv_bind_port, send_bind_port, recv_ttl, send_ttl
#
# Small file: 50 KiB -> ~35 parts at default MTU 1500.
run_test "small" 50    33401 33402 3 1

# Large file: 2 MiB -> ~1400 parts at default MTU 1500.
run_test "large" 2048  33403 33404 3 1

# Multicast: same protocol over an IP multicast group. Some CI/loopback setups
# lack multicast routing, so first probe whether a group datagram is delivered
# at all. Only when the environment supports it do we run the transfer as a hard
# test — so a real regression in the join/addressing logic fails the suite,
# rather than being masked as "unavailable".
mc_available() {
    command -v python3 >/dev/null 2>&1 || return 1
    python3 - <<'PY' 2>/dev/null
import socket, struct, sys
group, port = "239.255.42.99", 34099
try:
    r = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    r.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    r.bind(("", port))
    mreq = struct.pack("4sl", socket.inet_aton(group), socket.INADDR_ANY)
    r.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    r.settimeout(1.0)
    socket.socket(socket.AF_INET, socket.SOCK_DGRAM).sendto(b"probe", (group, port))
    sys.exit(0 if r.recvfrom(16)[0] == b"probe" else 1)
except Exception:
    sys.exit(1)
PY
}

if mc_available; then
    DEST="--multicast 239.255.42.99" run_test "multicast" 50 33405 33406 3 1
else
    echo "SKIP: [multicast] not available in this environment"
fi

# Resume: interrupt a slow receive with SIGINT, then finish it with --resume.
# Timing-based, so if no snapshot lands (transfer finished or nothing arrived in
# the window) it SKIPs; but once a snapshot exists, resume must succeed or FAIL.
run_resume_test() {
    local src="$WORKDIR/src-resume.bin"
    local rdir="$WORKDIR/resume"
    mkdir -p "$rdir"
    echo "==> [resume] generating 2 MiB file"
    dd if=/dev/urandom of="$src" bs=1024 count=2048 status=none

    echo "==> [resume] run 1: slow receive, interrupt mid-transfer"
    ( cd "$rdir" && exec "$BINARY" receive out.bin --to 127.0.0.1 \
          --bind-port 33407 --port 33408 --ttl 10 --resume --overwrite > r1.log 2>&1 ) &
    local rpid=$!
    sleep 1
    "$BINARY" send "$src" --to 127.0.0.1 --bind-port 33408 --port 33407 \
              --rate 5 --ttl 5 > "$WORKDIR/rsend1.log" 2>&1 &
    local spid=$!
    sleep 2
    kill -INT "$rpid" 2>/dev/null || true; wait "$rpid" 2>/dev/null || true
    kill "$spid" 2>/dev/null || true; wait "$spid" 2>/dev/null || true

    if [ ! -f "$rdir/out.bin.part.idx" ]; then
        echo "SKIP: [resume] no snapshot produced in the interrupt window"
        return 0
    fi

    echo "==> [resume] run 2: finish with --resume"
    ( cd "$rdir" && exec "$BINARY" receive out.bin --to 127.0.0.1 \
          --bind-port 33407 --port 33408 --ttl 4 --resume --overwrite > r2.log 2>&1 ) &
    rpid=$!
    sleep 1
    "$BINARY" send "$src" --to 127.0.0.1 --bind-port 33408 --port 33407 \
              --rate 100 --ttl 3 > "$WORKDIR/rsend2.log" 2>&1
    wait "$rpid"

    if ! grep -q "Resuming" "$rdir/r2.log"; then
        echo "FAIL: [resume] second run did not resume from the snapshot"
        tail -20 "$rdir/r2.log"
        return 1
    fi
    if [ ! -f "$rdir/out.bin" ] || ! cmp -s "$src" "$rdir/out.bin"; then
        echo "FAIL: [resume] resumed file does not match source"
        return 1
    fi
    echo "PASS: [resume] interrupted transfer resumed and verified"
}
run_resume_test

echo
echo "All E2E tests passed."
