/*
 * Fault-injection shim that forces pwrite(2) to fail with ENOSPC, so a test can
 * drive filecast's disk-backed receiver into its "Failed to write received data"
 * branch without needing a real full disk (which would mean root, a scratch
 * filesystem, and fragile per-FS sparse-file semantics).
 *
 * writePartAt() in src/Receiver.cpp is the only pwrite() caller on POSIX, so
 * intercepting pwrite() targets exactly the part-data writes and nothing else
 * (the .part.idx snapshot uses write(), file hashing uses read()). The receiver
 * opens and ftruncate()s the .part file first — those succeed — and only the
 * first payload write hits ENOSPC, which is precisely the disk-full path.
 *
 * Loaded around the receiver under test only, via LD_PRELOAD (Linux) or
 * DYLD_INSERT_LIBRARIES (macOS). FAILPWRITE_AFTER (env, default 0) lets that many
 * pwrite() calls succeed before the rest fail, so a test can pick "fail on the
 * very first part" (0) or "fail partway through" (>0).
 *
 * Written in C on purpose: the sanitizer CI job overrides CMAKE_CXX_FLAGS only,
 * so a C shim stays free of ASan/UBSan instrumentation and preloads cleanly into
 * the instrumented receiver (an instrumented preload lib would trip ASan's
 * "runtime does not come first" guard).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

typedef ssize_t (*pwrite_fn)(int, const void*, size_t, off_t);

/* -2 = threshold not yet read from the environment. */
static long g_allowed = -2;
static long g_seen = 0;

static long threshold(void) {
    if (g_allowed == -2) {
        const char* e = getenv("FAILPWRITE_AFTER");  // Flawfinder: ignore (test-only knob, parsed with strtol)
        g_allowed = (e && *e) ? strtol(e, NULL, 10) : 0;
    }
    return g_allowed;
}

/* The receive loop is single-threaded, so the un-guarded counter is safe. */
static ssize_t injected_pwrite(int fd, const void* buf, size_t count, off_t offset) {
    static pwrite_fn real = NULL;
    if (g_seen++ >= threshold()) {
        errno = ENOSPC;
        return -1;
    }
    if (!real) real = (pwrite_fn)dlsym(RTLD_NEXT, "pwrite");
    return real(fd, buf, count, offset);
}

#if defined(__APPLE__)
/*
 * macOS uses two-level namespaces, so a plain symbol override is ignored; the
 * __interpose section remaps callers of pwrite() to injected_pwrite() instead
 * (no DYLD_FORCE_FLAT_NAMESPACE needed). An __interpose entry is just the pair
 * {replacement, original}; a 2-element pointer array matches that layout and,
 * unlike a named struct, gives static analysers no "unused member" to flag.
 */
__attribute__((used, section("__DATA,__interpose")))
static const void* interpose_pwrite[2] = {
    (const void*)&injected_pwrite,
    (const void*)&pwrite,
};
#else
/*
 * On Linux the LD_PRELOAD symbol simply shadows libc's. glibc may resolve
 * pwrite() to pwrite64() under _FILE_OFFSET_BITS=64, so override both.
 */
ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset) {
    return injected_pwrite(fd, buf, count, offset);
}
ssize_t pwrite64(int fd, const void* buf, size_t count, off_t offset) {
    return injected_pwrite(fd, buf, count, offset);
}
#endif
