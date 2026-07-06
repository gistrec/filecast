#!/usr/bin/env bash
#
# Timeout-bound E2E tests: prove that a continuous UDP flood can no longer keep
# filecast alive forever. Both the receiver's main wait and the sender's
# resend-serving phase used to key their timeout off "recvfrom sat idle for a
# second", so any host sending faster than that froze the countdown. They now
# key off a wall-clock deadline, so a flood still lets them give up.
#
# Two scenarios:
#   * receiver: junk flood -> the receiver must still time out (exit 2).
#   * sender:   valid RESEND flood -> the sender must still stop at its absolute
#               resend-phase deadline (exit 0), after actually serving resends.
#
# Run via:
#   ctest --test-dir build --output-on-failure
#
# Or directly:
#   BINARY=build/filecast bash tests/e2e_flood.sh
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
    echo "Error: $PYTHON not found in PATH; the flooder needs Python 3." >&2
    exit 1
fi

WORKDIR="$(mktemp -d -t fb-e2e-flood.XXXXXX)"
FLOOD_PID=""
PROC_PID=""
PROXY_PID=""
SEND_PID=""

cleanup() {
    if [ -n "$PROC_PID"  ]; then kill "$PROC_PID"  2>/dev/null || true; fi
    if [ -n "$FLOOD_PID" ]; then kill "$FLOOD_PID" 2>/dev/null || true; fi
    if [ -n "$PROXY_PID" ]; then kill "$PROXY_PID" 2>/dev/null || true; fi
    if [ -n "$SEND_PID"  ]; then kill "$SEND_PID"  2>/dev/null || true; fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

# Wait up to $1 tenths of a second for pid $2 to exit. Prints "1" if it exited,
# "0" if it is still running when the budget runs out.
wait_for_exit() {
    local tenths="$1" pid="$2" i
    for ((i = 0; i < tenths; i++)); do
        kill -0 "$pid" 2>/dev/null || { echo 1; return; }
        sleep 0.1
    done
    echo 0
}

# --- Scenario 1: receiver under a junk flood --------------------------------
# A receiver waiting for a sender is buried under tiny junk datagrams. None are
# ours, so it stores nothing; with only a 2s ttl it must still time out (exit 2)
# well within our budget, rather than being kept alive by the busy socket.
receiver_garbage_flood() {
    local recv_bind=33601 send_target=33602
    local log="$WORKDIR/recv-flood.log"

    echo "==> [receiver] flooding junk at the receive port"
    "$PYTHON" "$SCRIPT_DIR/flood.py" garbage 127.0.0.1 "$recv_bind" 20 \
        > "$WORKDIR/flood-recv.log" 2>&1 &
    FLOOD_PID=$!
    sleep 0.3  # let the flood ramp up before the receiver binds

    echo "==> [receiver] starting receiver (ttl=2) under the flood"
    "$BINARY" receive "$WORKDIR/out.bin" --to 127.0.0.1 \
              --bind-port "$recv_bind" --port "$send_target" --ttl 2 --delay-ms 0 \
        > "$log" 2>&1 &
    PROC_PID=$!

    # The fix makes it exit ~ttl seconds in; allow a wide margin for CI jitter.
    local exited
    exited="$(wait_for_exit 80 "$PROC_PID")"  # up to 8s
    if [ "$exited" -eq 0 ]; then
        echo "FAIL: [receiver] still alive under the flood after 8s (ttl was 2)"
        tail -5 "$log"
        return 1
    fi
    local rc=0; wait "$PROC_PID" || rc=$?
    PROC_PID=""
    kill "$FLOOD_PID" 2>/dev/null || true; wait "$FLOOD_PID" 2>/dev/null || true
    FLOOD_PID=""

    if [ "$rc" -ne 2 ]; then
        echo "FAIL: [receiver] expected timeout exit 2, got $rc"
        tail -5 "$log"
        return 1
    fi
    echo "PASS: [receiver] timed out under a junk flood instead of hanging"
}

# --- Scenario 2: sender under a valid RESEND flood --------------------------
# The sender finishes a transfer (no real receiver) and enters its resend phase.
# A flooder sniffs the sender's cleartext session id and hammers it with valid
# RESENDs for part 0. Each refreshes the idle ttl, so only the absolute phase
# deadline can end it. With ttl=1 that deadline is 10s; it must exit (0) well
# before our budget, having actually served at least one resend.
sender_resend_flood() {
    local send_bind=33603 sniff_port=33604
    local src="$WORKDIR/src-flood.bin"
    local log="$WORKDIR/send-flood.log"

    dd if=/dev/urandom of="$src" bs=1024 count=50 status=none

    echo "==> [sender] sniffing the session id and flooding RESENDs"
    "$PYTHON" "$SCRIPT_DIR/flood.py" resend "$sniff_port" 127.0.0.1 "$send_bind" 40 \
        > "$WORKDIR/flood-send.log" 2>&1 &
    FLOOD_PID=$!
    sleep 0.3  # start sniffing before the sender's first ANNOUNCE

    echo "==> [sender] starting sender (ttl=1, resend deadline 10s) under the flood"
    SECONDS=0
    "$BINARY" send "$src" --to 127.0.0.1 \
              --bind-port "$send_bind" --port "$sniff_port" --ttl 1 --delay-ms 0 \
        > "$log" 2>&1 &
    PROC_PID=$!

    # Absolute deadline is 10s; give a wide margin. Without it the flood would
    # keep the resend phase alive indefinitely and this budget would run out.
    local exited
    exited="$(wait_for_exit 250 "$PROC_PID")"  # up to 25s
    if [ "$exited" -eq 0 ]; then
        echo "FAIL: [sender] still serving resends after 25s — no absolute deadline"
        tail -5 "$log"
        return 1
    fi
    local elapsed="$SECONDS"
    local rc=0; wait "$PROC_PID" || rc=$?
    PROC_PID=""
    kill "$FLOOD_PID" 2>/dev/null || true; wait "$FLOOD_PID" 2>/dev/null || true
    FLOOD_PID=""

    if [ "$rc" -ne 0 ]; then
        echo "FAIL: [sender] expected clean exit 0, got $rc"
        tail -5 "$log"
        return 1
    fi
    # The flood must have actually driven the resend path, or the test proved
    # nothing about the deadline that bounds it.
    if ! grep -q "Re-sent" "$log"; then
        echo "FAIL: [sender] resend phase was never exercised (flood did not engage)"
        tail -5 "$log"
        return 1
    fi
    echo "PASS: [sender] stopped at the resend deadline after ${elapsed}s under the flood"
}

# --- Scenario 3: receiver in RESEND recovery under a junk flood --------------
# A drop-proxy permanently strands ~1/3 of the parts, so the receiver takes the
# FINISH, finds parts missing, and enters the recovery phase. A junk flood on the
# receive port then keeps the recovery drain loop's recvfrom busy. Scenario 1
# already proves the *main* wait survives a flood; this proves the recovery loop
# does too — it must still give up on its wall-clock deadline (exit 2, reaching
# the "part(s) missing" recovery-timeout path) rather than spinning forever, which
# is what the pre-fix drain loop did (it only broke on an idle recvfrom).
receiver_recovery_flood() {
    local px=33605 recv_bind=33606 send_bind=33607
    local src="$WORKDIR/src-recov.bin"
    local rlog="$WORKDIR/recv-recov.log"

    dd if=/dev/urandom of="$src" bs=1024 count=60 status=none  # 60 KiB => 40 parts

    echo "==> [recovery] drop-proxy strands ~1/3 of the parts to force recovery"
    "$PYTHON" "$SCRIPT_DIR/dropproxy.py" "$px" 127.0.0.1 "$recv_bind" 3 \
        > "$WORKDIR/proxy-recov.log" 2>&1 &
    PROXY_PID=$!
    sleep 0.2  # let the proxy bind before the sender broadcasts

    echo "==> [recovery] starting receiver (ttl=4) behind the drop-proxy"
    "$BINARY" receive "$WORKDIR/out-recov.bin" --to 127.0.0.1 \
              --bind-port "$recv_bind" --port "$send_bind" --ttl 4 --delay-ms 0 --verbose \
        > "$rlog" 2>&1 &
    PROC_PID=$!
    sleep 0.3

    "$BINARY" send "$src" --to 127.0.0.1 \
              --bind-port "$send_bind" --port "$px" --ttl 1 --delay-ms 0 \
        > "$WORKDIR/send-recov.log" 2>&1 &
    SEND_PID=$!

    # Wait (bounded) for recovery to actually begin, then unleash the flood so it
    # lands while the receiver is in the recovery drain loop, not the main wait.
    local started=0 i
    for ((i = 0; i < 100; i++)); do
        if grep -q "Request part" "$rlog" 2>/dev/null; then started=1; break; fi
        sleep 0.05
    done
    if [ "$started" -ne 1 ]; then
        echo "FAIL: [recovery] receiver never entered recovery (test setup issue)"
        tail -5 "$rlog"
        return 1
    fi
    "$PYTHON" "$SCRIPT_DIR/flood.py" garbage 127.0.0.1 "$recv_bind" 20 \
        > "$WORKDIR/flood-recov.log" 2>&1 &
    FLOOD_PID=$!

    # The fix makes recovery give up ~ttl seconds in; allow a wide margin. Without
    # it the flood keeps the drain loop busy and this budget runs out.
    local exited
    exited="$(wait_for_exit 120 "$PROC_PID")"  # up to 12s
    if [ "$exited" -eq 0 ]; then
        echo "FAIL: [recovery] receiver wedged in the recovery drain loop under the flood"
        tail -5 "$rlog"
        return 1
    fi
    local rc=0; wait "$PROC_PID" || rc=$?
    PROC_PID=""
    kill "$FLOOD_PID" 2>/dev/null || true; wait "$FLOOD_PID" 2>/dev/null || true
    FLOOD_PID=""
    kill "$SEND_PID" "$PROXY_PID" 2>/dev/null || true
    wait "$SEND_PID" 2>/dev/null || true; wait "$PROXY_PID" 2>/dev/null || true
    SEND_PID=""; PROXY_PID=""

    if [ "$rc" -ne 2 ]; then
        echo "FAIL: [recovery] expected timeout exit 2, got $rc"
        tail -5 "$rlog"
        return 1
    fi
    # "part(s) missing" comes only from the recovery-timeout path, proving the
    # flood was survived there and not in the main wait.
    if ! grep -q "part(s) missing" "$rlog"; then
        echo "FAIL: [recovery] timed out in the wrong phase (not recovery)"
        tail -5 "$rlog"
        return 1
    fi
    echo "PASS: [recovery] recovery loop timed out under a junk flood instead of hanging"
}

receiver_garbage_flood
sender_resend_flood
receiver_recovery_flood

echo
echo "All flood E2E tests passed."
