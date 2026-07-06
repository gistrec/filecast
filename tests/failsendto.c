/*
 * Fault-injection shim that forces sendto(2) to fail with ENETDOWN, so a test
 * can drive filecast's sender into its "the network is unreachable" branches
 * without taking a real interface down (which would need root and would break
 * loopback for the rest of the suite).
 *
 * The sender's only sendto() callers are the packet broadcasts in Sender.cpp
 * (ANNOUNCE, TRANSFER, FINISH); socket setup uses bind()/setsockopt(). The shim
 * matches on the filecast header (magic "FCST"(4) + version(1) + type(1)) so it
 * only ever fails our own datagrams. FAILSENDTO_TYPE (env) selects which packet
 * type fails: 0 (default) fails every filecast packet — exercising the
 * all-ANNOUNCE-failed path — while 2 fails only TRANSFER packets, letting the
 * ANNOUNCE through so the transfer-loop "sent nothing" path is hit instead.
 *
 * Written in C for the same reason as tests/failpwrite.c: the sanitizer CI job
 * instruments C++ only, so the shim preloads cleanly ahead of the ASan runtime.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

typedef ssize_t (*sendto_fn)(int, const void*, size_t, int,
                             const struct sockaddr*, socklen_t);

/* -1 = threshold not yet read from the environment. */
static int g_target = -1;

static int target(void) {
    if (g_target == -1) {
        const char* e = getenv("FAILSENDTO_TYPE");  /* Flawfinder: ignore (test-only knob) */
        g_target = (e && *e) ? (int)strtol(e, NULL, 10) : 0;
    }
    return g_target;
}

/* filecast header: magic "FCST"(4) + version(1) + type(1) at offset 5. */
static int should_fail(const void* buf, size_t len) {
    const unsigned char* p = (const unsigned char*)buf;
    if (len < 6 || memcmp(p, "FCST", 4) != 0) return 0;
    int t = target();
    return t == 0 || (int)p[5] == t;
}

/*
 * Call the real sendto(). On macOS the __interpose tuple below already pairs us
 * with the genuine symbol, so referencing sendto() here reaches libc directly;
 * dlsym(RTLD_NEXT) would instead resolve back to this shim and tail-recurse.
 * On Linux our exported sendto() shadows libc's, so RTLD_NEXT is required.
 */
static ssize_t call_real(int fd, const void* buf, size_t len, int flags,
                         const struct sockaddr* dst, socklen_t dlen) {
#if defined(__APPLE__)
    return sendto(fd, buf, len, flags, dst, dlen);
#else
    static sendto_fn real = NULL;
    if (!real) real = (sendto_fn)dlsym(RTLD_NEXT, "sendto");
    return real(fd, buf, len, flags, dst, dlen);
#endif
}

static ssize_t injected_sendto(int fd, const void* buf, size_t len, int flags,
                               const struct sockaddr* dst, socklen_t dlen) {
    if (should_fail(buf, len)) {
        errno = ENETDOWN;
        return -1;
    }
    return call_real(fd, buf, len, flags, dst, dlen);
}

#if defined(__APPLE__)
__attribute__((used, section("__DATA,__interpose")))
static const void* interpose_sendto[2] = {
    (const void*)&injected_sendto,
    (const void*)&sendto,
};
#else
ssize_t sendto(int fd, const void* buf, size_t len, int flags,
               const struct sockaddr* dst, socklen_t dlen) {
    return injected_sendto(fd, buf, len, flags, dst, dlen);
}
#endif
