#!/usr/bin/env bash
#
# Corrupting end-to-end test: run a transfer through a proxy that flips one
# payload byte of a single TRANSFER packet, so every part is delivered (nothing
# is re-requested) but the reassembled file fails its announced SHA-256. This
# pins verifyAndWrite's checksum-mismatch cleanup — the branch that decides
# between discardSnapshot() (--resume) and removePartFiles() (plain receive):
#
#   * the receiver must exit 2 and report the mismatch,
#   * it must NOT write the corrupt bytes to the output file, and
#   * it must leave NO .part / .part.idx behind in EITHER mode — a resumed run
#     drops the poisoned snapshot too, so the next --resume starts clean instead
#     of reloading the same corruption forever.
#
# Run via:
#   ctest --test-dir build --output-on-failure
#
# Or directly:
#   BINARY=build/filecast bash tests/e2e_corrupt.sh
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
    echo "Error: $PYTHON not found in PATH; corrupt proxy needs Python 3." >&2
    exit 1
fi

WORKDIR="$(mktemp -d -t fb-e2e-corrupt.XXXXXX)"
PROXY_PID=""
RECV_PID=""
SEND_PID=""

cleanup() {
    [ -n "$RECV_PID"  ] && kill "$RECV_PID"  2>/dev/null || true
    [ -n "$SEND_PID"  ] && kill "$SEND_PID"  2>/dev/null || true
    [ -n "$PROXY_PID" ] && kill "$PROXY_PID" 2>/dev/null || true
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

# Topology (all on 127.0.0.1):
#   sender bind   = 33602, sender   target -> 33603 (proxy fwd in, corrupts once)
#   receiver bind = 33601, receiver target -> 33604 (proxy back in, clean)
#   proxy fwd:  33603 -> 33601  (sender->receiver, one payload byte flipped)
#   proxy back: 33604 -> 33602  (receiver->sender, untouched)
RECV_BIND=33601
SEND_BIND=33602
PROXY_FWD_IN=33603
PROXY_BACK_IN=33604

# Args: label, extra receiver flag (e.g. "--resume" or "")
run_corrupt_test() {
    local label="$1"
    local extra_flag="$2"
    # Optional receiver flag as an array so an empty value expands to no argument
    # (a quoted "" would be passed as a stray empty positional). The +-guard keeps
    # the expansion safe under `set -u` on bash 3.2 (macOS's default).
    local -a extra=()
    [ -n "$extra_flag" ] && extra=("$extra_flag")

    local dir="$WORKDIR/$label"
    mkdir -p "$dir"
    local src="$dir/src.bin"
    local recv_log="$dir/recv.log"
    local send_log="$dir/send.log"
    local proxy_log="$dir/proxy.log"

    echo "==> [$label] generating 50 KiB file (one payload byte will be corrupted)"
    dd if=/dev/urandom of="$src" bs=1024 count=50 status=none

    echo "==> [$label] starting corrupt proxy"
    "$PYTHON" "$SCRIPT_DIR/corrupt_proxy.py" \
        --listen-fwd  "$PROXY_FWD_IN"  --target-fwd  "$RECV_BIND" \
        --listen-back "$PROXY_BACK_IN" --target-back "$SEND_BIND" \
        > "$proxy_log" 2>&1 &
    PROXY_PID=$!
    sleep 0.5  # let the proxy bind both sockets

    echo "==> [$label] starting receiver (${extra_flag:-plain})"
    # The .part/.part.idx snapshot lands next to the output file, so an absolute
    # output path keeps every artifact inside $dir without cd-ing (which would
    # break a relative BINARY like build/filecast).
    "$BINARY" receive "$dir/out.bin" --to 127.0.0.1 \
              --bind-port "$RECV_BIND" --port "$PROXY_BACK_IN" \
              --ttl 10 --delay-ms 0 ${extra[@]+"${extra[@]}"} > "$recv_log" 2>&1 &
    RECV_PID=$!
    sleep 1

    echo "==> [$label] starting sender"
    "$BINARY" send "$src" --to 127.0.0.1 \
              --bind-port "$SEND_BIND" --port "$PROXY_FWD_IN" \
              --ttl 5 --delay-ms 0 > "$send_log" 2>&1 &
    SEND_PID=$!

    # The receiver reassembles every part, fails the whole-file checksum, and
    # exits 2. Capture that exit code rather than letting `wait` abort the script.
    local rc=0
    wait "$RECV_PID" || rc=$?
    RECV_PID=""

    kill "$SEND_PID"  2>/dev/null || true; wait "$SEND_PID"  2>/dev/null || true; SEND_PID=""
    kill "$PROXY_PID" 2>/dev/null || true; wait "$PROXY_PID" 2>/dev/null || true; PROXY_PID=""

    if [ "$rc" -ne 2 ]; then
        echo "FAIL: [$label] expected receiver exit 2, got $rc"
        echo "--- receiver log (tail):"; tail -20 "$recv_log"
        echo "--- proxy log (tail):";    tail -20 "$proxy_log"
        return 1
    fi
    if ! grep -q "checksum mismatch" "$recv_log"; then
        echo "FAIL: [$label] receiver did not report a checksum mismatch"
        echo "--- receiver log (tail):"; tail -20 "$recv_log"
        echo "--- proxy log (tail):";    tail -20 "$proxy_log"
        return 1
    fi
    # Sanity: if the proxy never corrupted a packet the file would have verified,
    # so this test would pass for the wrong reason. Require proof it did its job.
    if ! grep -q "flipped one payload byte" "$proxy_log"; then
        echo "FAIL: [$label] proxy never corrupted a TRANSFER packet"
        echo "--- proxy log (tail):"; tail -20 "$proxy_log"
        return 1
    fi
    if [ -f "$dir/out.bin" ]; then
        echo "FAIL: [$label] corrupt data was written to the output file"
        return 1
    fi
    if [ -f "$dir/out.bin.part" ] || [ -f "$dir/out.bin.part.idx" ]; then
        echo "FAIL: [$label] left a poisoned snapshot behind:"
        ls "$dir"
        return 1
    fi

    echo "PASS: [$label] checksum mismatch rejected (exit 2, no output, no snapshot)"
}

# Plain receive: mismatch -> removePartFiles() wipes the .part storage.
run_corrupt_test "corrupt-plain"  ""
# Resumable receive: mismatch -> discardSnapshot() drops the poisoned snapshot
# too, so a later --resume does not reload the same corruption forever.
run_corrupt_test "corrupt-resume" "--resume"

echo
echo "All corrupt E2E tests passed."
