#!/usr/bin/env bash
#
# Durable-sync-failure end-to-end test: run a normal loopback transfer where
# every part is delivered and written, but preload a shim (tests/failsync) into
# the RECEIVER so its durable sync (fsync) fails with EIO. This pins the fix that
# stops verifyAndWrite from announcing "sha256 verified" for data that never
# reached the disk: the checksum re-read is served from the page cache and would
# pass, so the failed sync must be caught at close time. The receiver must
#
#   * exit 2 and report "Failed to flush received data to disk",
#   * NOT print "sha256 verified",
#   * NOT produce the output file, and
#   * leave no .part / .part.idx behind in EITHER mode (the flush failure cleans
#     up exactly like a checksum mismatch: removePartFiles / discardSnapshot).
#
# Linux only: durableSync() uses fsync() here, which the shim shadows cleanly.
# macOS flushes with fcntl(F_FULLFSYNC) instead, and interposing fcntl process-
# wide is too fragile to fault-inject reliably, so the test self-skips there (the
# receiver-side code being pinned is platform-independent).
#
# Run via:
#   ctest --test-dir build --output-on-failure
#
# Or directly:
#   BINARY=build/filecast FAILLIB=build/libfailsync.so bash tests/e2e_syncfail.sh
#
set -euo pipefail

if [ "$(uname -s)" != "Linux" ]; then
    echo "SKIP: durable-sync fault injection is Linux-only (see script header)."
    exit 0
fi

BINARY="${BINARY:-./filecast}"
FAILLIB="${FAILLIB:-}"

if [ ! -x "$BINARY" ]; then
    echo "Error: $BINARY not found or not executable. Build first." >&2
    exit 1
fi
if [ -z "$FAILLIB" ] || [ ! -f "$FAILLIB" ]; then
    echo "Error: FAILLIB ($FAILLIB) not found; build the failsync shim first." >&2
    exit 1
fi

PRELOAD_VAR="LD_PRELOAD"

# An ASan build aborts when an interceptor shim is preloaded ahead of the ASan
# runtime ("runtime does not come first"); waive just that check, keeping every
# other ASan check active. Appended to any ASAN_OPTIONS the job already set.
RECV_ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}verify_asan_link_order=0"

WORKDIR="$(mktemp -d -t fb-e2e-syncfail.XXXXXX)"
RECV_PID=""
SEND_PID=""

cleanup() {
    if [ -n "$RECV_PID" ]; then kill "$RECV_PID" 2>/dev/null || true; fi
    if [ -n "$SEND_PID" ]; then kill "$SEND_PID" 2>/dev/null || true; fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

# Direct loopback — the fault is injected locally, so no proxy is involved.
RECV_BIND=33801
SEND_BIND=33802

# Args: label, extra receiver flag (e.g. "--resume" or "")
run_syncfail_test() {
    local label="$1"
    local extra_flag="$2"
    # Optional receiver flag as an array so an empty value expands to no argument.
    # The +-guard keeps the expansion safe under `set -u` on bash 3.2 (macOS).
    local -a extra=()
    [ -n "$extra_flag" ] && extra=("$extra_flag")

    local dir="$WORKDIR/$label"
    mkdir -p "$dir"
    local src="$dir/src.bin"
    local recv_log="$dir/recv.log"
    local send_log="$dir/send.log"

    echo "==> [$label] generating 20 KiB file (all parts land, the sync will fail)"
    dd if=/dev/urandom of="$src" bs=1024 count=20 status=none

    echo "==> [$label] starting receiver (${extra_flag:-plain}) with sync->EIO shim"
    env "$PRELOAD_VAR=$FAILLIB" ASAN_OPTIONS="$RECV_ASAN_OPTIONS" \
        "$BINARY" receive "$dir/out.bin" --to 127.0.0.1 \
                  --bind-port "$RECV_BIND" --port "$SEND_BIND" \
                  --ttl 10 --delay-ms 0 ${extra[@]+"${extra[@]}"} > "$recv_log" 2>&1 &
    RECV_PID=$!
    sleep 1

    echo "==> [$label] starting sender"
    "$BINARY" send "$src" --to 127.0.0.1 \
              --bind-port "$SEND_BIND" --port "$RECV_BIND" \
              --ttl 5 --delay-ms 0 > "$send_log" 2>&1 &
    SEND_PID=$!

    # The receiver reassembles every part, fails the durable sync, and exits 2.
    # Capture that code rather than letting `wait` abort the script under `set -e`.
    local rc=0
    wait "$RECV_PID" || rc=$?
    RECV_PID=""

    kill "$SEND_PID" 2>/dev/null || true; wait "$SEND_PID" 2>/dev/null || true; SEND_PID=""

    if [ "$rc" -ne 2 ]; then
        echo "FAIL: [$label] expected receiver exit 2, got $rc"
        echo "--- receiver log (tail):"; tail -20 "$recv_log"
        return 1
    fi
    if ! grep -q "Failed to flush received data to disk" "$recv_log"; then
        echo "FAIL: [$label] receiver did not report a flush failure"
        echo "--- receiver log (tail):"; tail -20 "$recv_log"
        return 1
    fi
    # The core regression: a failed sync must never be announced as verified.
    if grep -q "sha256 verified" "$recv_log"; then
        echo "FAIL: [$label] receiver reported 'sha256 verified' despite the failed sync"
        echo "--- receiver log (tail):"; tail -20 "$recv_log"
        return 1
    fi
    if [ -f "$dir/out.bin" ]; then
        echo "FAIL: [$label] output file was written despite the flush failure"
        return 1
    fi
    # A flush failure cleans up like a checksum mismatch: neither mode keeps a
    # .part or a .part.idx, since the bytes were never durably persisted.
    if [ -f "$dir/out.bin.part" ] || [ -f "$dir/out.bin.part.idx" ]; then
        echo "FAIL: [$label] left a .part/.part.idx behind:"
        ls "$dir"
        return 1
    fi

    echo "PASS: [$label] flush failure rejected (exit 2, not verified, no output)"
}

# Plain receive: flush failure -> removePartFiles() wipes the .part storage.
run_syncfail_test "syncfail-plain"  ""
# Resumable receive: flush failure -> discardSnapshot() drops the unreliable
# .part/.part.idx rather than advertising un-synced parts to a later --resume.
run_syncfail_test "syncfail-resume" "--resume"

echo
echo "All durable-sync-failure E2E tests passed."
