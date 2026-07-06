#!/usr/bin/env bash
#
# Disk-full end-to-end test: run a normal loopback transfer, but preload a shim
# (tests/failpwrite) into the RECEIVER so its first pwrite() of received data
# fails with ENOSPC — exactly what a full disk does. This pins the write-failure
# cleanup branch that mirrors the checksum-mismatch one (see e2e_corrupt.sh):
# handleTransfer sets storage_failed and the receiver must
#
#   * exit 2 and report "Failed to write received data",
#   * NOT produce the output file, and
#   * clean up according to mode — a plain receive drops the .part storage
#     entirely (removePartFiles), while a --resume receive keeps the .part so a
#     later run can retry, but never leaves a half-written .part.idx snapshot.
#
# The shim only intercepts pwrite(), which on POSIX is writePartAt()'s sole
# syscall, so the open()/ftruncate() that create the .part file still succeed and
# the failure lands precisely on the payload write — the real disk-full path.
#
# Run via:
#   ctest --test-dir build --output-on-failure
#
# Or directly:
#   BINARY=build/filecast FAILLIB=build/libfailpwrite.so bash tests/e2e_diskfull.sh
#
set -euo pipefail

BINARY="${BINARY:-./filecast}"
FAILLIB="${FAILLIB:-}"

if [ ! -x "$BINARY" ]; then
    echo "Error: $BINARY not found or not executable. Build first." >&2
    exit 1
fi
if [ -z "$FAILLIB" ] || [ ! -f "$FAILLIB" ]; then
    echo "Error: FAILLIB ($FAILLIB) not found; build the failpwrite shim first." >&2
    exit 1
fi

# LD_PRELOAD on Linux, DYLD_INSERT_LIBRARIES on macOS. The variable is applied
# only to the receiver command, so the sender and shell are never affected.
case "$(uname -s)" in
    Darwin) PRELOAD_VAR="DYLD_INSERT_LIBRARIES" ;;
    *)      PRELOAD_VAR="LD_PRELOAD" ;;
esac

# A binary built with AddressSanitizer (the sanitizer CI job) aborts on startup
# when an interceptor shim is LD_PRELOADed ahead of the ASan runtime ("runtime
# does not come first"). verify_asan_link_order=0 waives just that check while
# keeping every other ASan check active. Appended to any ASAN_OPTIONS the job
# already set, and ignored entirely by non-ASan builds.
RECV_ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}verify_asan_link_order=0"

WORKDIR="$(mktemp -d -t fb-e2e-diskfull.XXXXXX)"
RECV_PID=""
SEND_PID=""

cleanup() {
    if [ -n "$RECV_PID" ]; then kill "$RECV_PID" 2>/dev/null || true; fi
    if [ -n "$SEND_PID" ]; then kill "$SEND_PID" 2>/dev/null || true; fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

# Receiver binds $RECV_BIND and sends RESEND back to the sender's $SEND_BIND;
# the sender is the mirror image. Direct loopback — the fault is injected
# locally, so no proxy is involved.
RECV_BIND=33701
SEND_BIND=33702

# Args: label, extra receiver flag (e.g. "--resume" or "")
run_diskfull_test() {
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

    echo "==> [$label] generating 20 KiB file (its first part write will fail)"
    dd if=/dev/urandom of="$src" bs=1024 count=20 status=none

    echo "==> [$label] starting receiver (${extra_flag:-plain}) with pwrite->ENOSPC shim"
    # Absolute output path keeps the .part/.part.idx artifacts inside $dir. The
    # preload var is prefixed onto the receiver command only.
    env "$PRELOAD_VAR=$FAILLIB" ASAN_OPTIONS="$RECV_ASAN_OPTIONS" FAILPWRITE_AFTER=0 \
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

    # The receiver hits ENOSPC on the first payload write and exits 2. Capture
    # that code rather than letting `wait` abort the script under `set -e`.
    local rc=0
    wait "$RECV_PID" || rc=$?
    RECV_PID=""

    kill "$SEND_PID" 2>/dev/null || true; wait "$SEND_PID" 2>/dev/null || true; SEND_PID=""

    if [ "$rc" -ne 2 ]; then
        echo "FAIL: [$label] expected receiver exit 2, got $rc"
        echo "--- receiver log (tail):"; tail -20 "$recv_log"
        return 1
    fi
    if ! grep -q "Failed to write received data" "$recv_log"; then
        echo "FAIL: [$label] receiver did not report a write failure"
        echo "--- receiver log (tail):"; tail -20 "$recv_log"
        return 1
    fi
    if [ -f "$dir/out.bin" ]; then
        echo "FAIL: [$label] output file was written despite the write failure"
        return 1
    fi
    # A poisoned/torn snapshot must never survive: neither mode may leave a
    # .part.idx behind, since a half-transfer that never wrote a byte has no
    # resumable state to record.
    if [ -f "$dir/out.bin.part.idx" ]; then
        echo "FAIL: [$label] left a .part.idx snapshot behind:"
        ls "$dir"
        return 1
    fi
    # Mode-specific .part handling: plain receive wipes it (removePartFiles);
    # --resume keeps the (sparse, unwritten) .part so a later retry can reuse it
    # instead of reallocating from scratch.
    if [ -n "$extra_flag" ]; then
        if [ ! -f "$dir/out.bin.part" ]; then
            echo "FAIL: [$label] --resume should keep the .part for a later retry"
            ls "$dir"
            return 1
        fi
    else
        if [ -f "$dir/out.bin.part" ]; then
            echo "FAIL: [$label] plain receive left a .part behind:"
            ls "$dir"
            return 1
        fi
    fi

    echo "PASS: [$label] write failure rejected (exit 2, no output, correct cleanup)"
}

# Plain receive: write failure -> removePartFiles() wipes the .part storage.
run_diskfull_test "diskfull-plain"  ""
# Resumable receive: write failure keeps the .part (retryable) but writes no
# .part.idx snapshot for a transfer that never stored a byte.
run_diskfull_test "diskfull-resume" "--resume"

echo
echo "All disk-full E2E tests passed."
