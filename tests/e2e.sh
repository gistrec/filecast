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

# --iface guard rails: the flag only applies to multicast and must be a valid
# IPv4. These are pure argument checks (no sockets), so they run everywhere and
# pin the validation added alongside --iface. Assert the *specific* rejection
# message rather than just a non-zero exit — otherwise, on a host without
# multicast routing, a broken validation could fall through to a failed group
# join (also exit 1) and pass for the wrong reason.
run_iface_validation() {
    local f="$WORKDIR/iface-src.bin"
    echo "iface-test" > "$f"
    local out=0 err

    err="$("$BINARY" send "$f" --iface 10.0.0.1 2>&1 >/dev/null || true)"
    grep -q "only applies to --multicast" <<<"$err" || {
        echo "FAIL: [iface] --iface without --multicast not rejected: $err"; out=1; }

    err="$("$BINARY" send "$f" --to 127.0.0.1 --iface 10.0.0.1 2>&1 >/dev/null || true)"
    grep -q "only applies to --multicast" <<<"$err" || {
        echo "FAIL: [iface] --iface with unicast --to not rejected: $err"; out=1; }

    err="$("$BINARY" send "$f" --multicast 239.1.2.3 --iface not-an-ip 2>&1 >/dev/null || true)"
    grep -q -- "--iface must be a valid IPv4 address" <<<"$err" || {
        echo "FAIL: [iface] invalid --iface IPv4 not rejected: $err"; out=1; }

    [ "$out" -eq 0 ] && echo "PASS: [iface] --iface guard rails reject misuse"
    return "$out"
}
run_iface_validation

# L4 regression: an --mtu whose datagram (chunk + 18-byte header) exceeds the
# host's UDP send limit must be rejected up front, never reported as a successful
# (but silently truncated) transfer. macOS/BSD cap outbound datagrams at
# net.inet.udp.maxdgram (~9216); elsewhere the 65507 IPv4 ceiling applies and the
# static --mtu cap (65489) bites. Pure validation, so it runs everywhere.
run_mtu_datagram_limit_test() {
    local f="$WORKDIR/mtu-src.bin"
    echo "mtu-test" > "$f"
    local out=0 err header=18 limit
    limit="$(sysctl -n net.inet.udp.maxdgram 2>/dev/null || echo 65507)"
    local too_big=$(( limit - header + 1 ))                       # datagram = limit + 1
    local deliverable=$(( (limit < 65507 ? limit : 65507) - header ))

    # Oversized: must fail loudly with an --mtu error, never print "Sent ".
    err="$("$BINARY" send "$f" --to 127.0.0.1 --ttl 1 --delay-ms 0 --mtu "$too_big" 2>&1 || true)"
    if grep -q "Sent " <<<"$err"; then
        echo "FAIL: [mtu-limit] oversized --mtu $too_big (datagram $((too_big+header)) > $limit) reported success"; out=1
    elif ! grep -q -- "--mtu" <<<"$err"; then
        echo "FAIL: [mtu-limit] oversized --mtu $too_big rejected without an --mtu error: $err"; out=1
    fi

    # Largest deliverable --mtu must still be accepted.
    if ! "$BINARY" send "$f" --to 127.0.0.1 --ttl 1 --delay-ms 0 --mtu "$deliverable" >/dev/null 2>&1; then
        echo "FAIL: [mtu-limit] largest deliverable --mtu $deliverable was rejected"; out=1
    fi

    [ "$out" -eq 0 ] && echo "PASS: [mtu-limit] oversized --mtu rejected, deliverable --mtu accepted"
    return "$out"
}
run_mtu_datagram_limit_test

# Announced-name delivery: run `receive` with NO output path, so the file must
# be saved under the name carried in the ANNOUNCE. This is the only test that
# exercises the name_len/name wire fields — every other case passes an explicit
# output path, which discards the announced name.
run_announced_name_test() {
    local dir="$WORKDIR/announced-name"
    mkdir -p "$dir/send" "$dir/recv"
    dd if=/dev/urandom of="$dir/send/announced-name.bin" bs=1024 count=50 status=none

    echo "==> [name] starting receiver with no output path"
    ( cd "$dir/recv" && exec "$BINARY" receive --to 127.0.0.1 \
          --bind-port 33409 --port 33410 --ttl 5 --delay-ms 0 > recv.log 2>&1 ) &
    local rpid=$!
    sleep 1
    "$BINARY" send "$dir/send/announced-name.bin" --to 127.0.0.1 \
              --bind-port 33410 --port 33409 --ttl 2 --delay-ms 0 \
              > "$dir/send.log" 2>&1
    if ! wait "$rpid"; then
        echo "FAIL: [name] receiver exited non-zero"
        cat "$dir/recv/recv.log"
        return 1
    fi
    if [ ! -f "$dir/recv/announced-name.bin" ]; then
        echo "FAIL: [name] file was not saved under the announced name"
        ls "$dir/recv"; tail -5 "$dir/recv/recv.log"
        return 1
    fi
    if ! cmp -s "$dir/send/announced-name.bin" "$dir/recv/announced-name.bin"; then
        echo "FAIL: [name] file saved under the announced name does not match source"
        return 1
    fi
    echo "PASS: [name] file delivered under its announced name"
}
run_announced_name_test

# No-clobber: an existing output file must survive a receive without
# --overwrite (exit 2, byte-identical content, no leftover temp files), and be
# replaced once --overwrite is given. Pins the no-clobber contract that the
# atomic finalize (finalizeNoClobber) enforces even against files that appear
# mid-write.
run_no_clobber_test() {
    local dir="$WORKDIR/no-clobber"
    mkdir -p "$dir"
    dd if=/dev/urandom of="$dir/src.bin" bs=1024 count=50 status=none
    echo "precious local data" > "$dir/out.bin"
    cp "$dir/out.bin" "$dir/expected.bin"

    echo "==> [no-clobber] receive onto an existing file without --overwrite"
    ( cd "$dir" && exec "$BINARY" receive out.bin --to 127.0.0.1 \
          --bind-port 33411 --port 33412 --ttl 5 --delay-ms 0 > recv1.log 2>&1 ) &
    local rpid=$!
    sleep 1
    "$BINARY" send "$dir/src.bin" --to 127.0.0.1 --bind-port 33412 --port 33411 \
              --ttl 2 --delay-ms 0 > "$dir/send1.log" 2>&1

    local rc=0
    wait "$rpid" || rc=$?
    if [ "$rc" -ne 2 ]; then
        echo "FAIL: [no-clobber] expected receiver exit 2, got $rc"
        tail -5 "$dir/recv1.log"
        return 1
    fi
    if ! grep -q "already exists" "$dir/recv1.log"; then
        echo "FAIL: [no-clobber] missing 'already exists' error"
        tail -5 "$dir/recv1.log"
        return 1
    fi
    if ! cmp -s "$dir/expected.bin" "$dir/out.bin"; then
        echo "FAIL: [no-clobber] existing file was modified"
        return 1
    fi
    if ls "$dir"/out.bin.part.* >/dev/null 2>&1; then
        echo "FAIL: [no-clobber] leftover temp files:"
        ls "$dir"
        return 1
    fi

    echo "==> [no-clobber] same receive with --overwrite replaces the file"
    ( cd "$dir" && exec "$BINARY" receive out.bin --to 127.0.0.1 \
          --bind-port 33411 --port 33412 --ttl 5 --delay-ms 0 --overwrite \
          > recv2.log 2>&1 ) &
    rpid=$!
    sleep 1
    "$BINARY" send "$dir/src.bin" --to 127.0.0.1 --bind-port 33412 --port 33411 \
              --ttl 2 --delay-ms 0 > "$dir/send2.log" 2>&1
    if ! wait "$rpid"; then
        echo "FAIL: [no-clobber] receive with --overwrite exited non-zero"
        tail -5 "$dir/recv2.log"
        return 1
    fi
    if ! cmp -s "$dir/src.bin" "$dir/out.bin"; then
        echo "FAIL: [no-clobber] --overwrite did not replace the file"
        return 1
    fi
    echo "PASS: [no-clobber] refused without --overwrite, replaced with it"
}
run_no_clobber_test

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

# Timeout before ANNOUNCE: a `receive` with no output path that times out before
# any sender appears must not touch the cwd. Previously fileName was still empty
# at that point, so the cleanup removed a bare ".part"/".part.idx" — deleting an
# unrelated transfer's files. Plant decoys named exactly that and assert they
# survive (exit 2, no output produced).
run_timeout_no_sender_test() {
    local dir="$WORKDIR/timeout-no-sender"
    mkdir -p "$dir"
    echo "unrelated part data" > "$dir/.part"
    echo "unrelated idx data"  > "$dir/.part.idx"

    echo "==> [no-sender] receive with no output path, no sender, short ttl"
    local rc=0
    ( cd "$dir" && exec "$BINARY" receive --to 127.0.0.1 \
          --bind-port 33413 --port 33414 --ttl 1 --delay-ms 0 > recv.log 2>&1 ) || rc=$?

    if [ "$rc" -ne 2 ]; then
        echo "FAIL: [no-sender] expected receiver exit 2, got $rc"
        tail -5 "$dir/recv.log"
        return 1
    fi
    if [ ! -f "$dir/.part" ] || [ ! -f "$dir/.part.idx" ]; then
        echo "FAIL: [no-sender] cwd .part/.part.idx were deleted by the timeout cleanup"
        ls -a "$dir"
        return 1
    fi
    echo "PASS: [no-sender] timeout before ANNOUNCE left the cwd untouched"
}
run_timeout_no_sender_test

# Pre-planted hardlink: O_NOFOLLOW blocks a symlink but not a hardlink, so a
# local attacker who can write the cwd could hardlink the predictable <name>.part
# onto a victim file and have the receiver's truncate/writes corrupt it. Plant
# such a hardlink and assert the receiver refuses (non-zero exit) and leaves the
# victim's contents byte-for-byte intact.
run_hardlink_part_test() {
    local dir="$WORKDIR/hardlink-part"
    mkdir -p "$dir"
    dd if=/dev/urandom of="$dir/src.bin" bs=1024 count=50 status=none
    printf 'precious victim data that must not be truncated' > "$dir/victim"
    cp "$dir/victim" "$dir/victim.expected"
    ln "$dir/victim" "$dir/out.bin.part"   # hardlink at the predictable .part name

    echo "==> [hardlink] receive with a hardlinked out.bin.part planted"
    local rc=0
    ( cd "$dir" && exec "$BINARY" receive out.bin --to 127.0.0.1 \
          --bind-port 33417 --port 33418 --ttl 5 --delay-ms 0 > recv.log 2>&1 ) &
    local rpid=$!
    sleep 1
    "$BINARY" send "$dir/src.bin" --to 127.0.0.1 --bind-port 33418 --port 33417 \
              --ttl 2 --delay-ms 0 > "$dir/send.log" 2>&1 || true
    wait "$rpid" || rc=$?

    if [ "$rc" -eq 0 ]; then
        echo "FAIL: [hardlink] receiver accepted a hardlinked .part (exit 0)"
        tail -5 "$dir/recv.log"
        return 1
    fi
    if ! cmp -s "$dir/victim.expected" "$dir/victim"; then
        echo "FAIL: [hardlink] victim file was corrupted through the hardlink"
        return 1
    fi
    echo "PASS: [hardlink] refused the pre-planted hardlink, victim intact"
}
run_hardlink_part_test

# Many parts: a tiny MTU splits even a small file into hundreds of parts, so this
# drives the received-parts bitmap (set/has/count) across a large index range and
# checks reassembly stays byte-exact. The bitmap replaced an std::set that scaled
# at ~48 bytes/part; this pins that the compact representation is still correct.
run_small_mtu_test() {
    local dir="$WORKDIR/small-mtu"
    mkdir -p "$dir"
    # 64 KiB at a 64-byte MTU -> ~1024 parts.
    dd if=/dev/urandom of="$dir/src.bin" bs=1024 count=64 status=none

    echo "==> [small-mtu] receiver with --mtu 64 (~1024 parts)"
    ( cd "$dir" && exec "$BINARY" receive out.bin --to 127.0.0.1 \
          --bind-port 33415 --port 33416 --ttl 5 --mtu 64 --delay-ms 0 > recv.log 2>&1 ) &
    local rpid=$!
    sleep 1
    "$BINARY" send "$dir/src.bin" --to 127.0.0.1 --bind-port 33416 --port 33415 \
              --ttl 2 --mtu 64 --delay-ms 0 > "$dir/send.log" 2>&1

    if ! wait "$rpid"; then
        echo "FAIL: [small-mtu] receiver exited non-zero"
        tail -20 "$dir/recv.log"
        return 1
    fi
    if [ ! -f "$dir/out.bin" ] || ! cmp -s "$dir/src.bin" "$dir/out.bin"; then
        echo "FAIL: [small-mtu] received file does not match source"
        return 1
    fi
    if ! grep -q "sha256 verified" "$dir/recv.log"; then
        echo "FAIL: [small-mtu] receiver did not verify the file checksum"
        tail -20 "$dir/recv.log"
        return 1
    fi
    echo "PASS: [small-mtu] ~1024-part transfer reassembled and verified"
}
run_small_mtu_test

echo
echo "All E2E tests passed."
