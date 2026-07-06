/*
 * Fault-injection shim that forces fsync(2) to fail with EIO, so a test can prove
 * verifyAndWrite() no longer reports "sha256 verified" for data that never
 * reached the disk. durableSync() in src/Receiver.cpp flushes with fsync() on
 * Linux (F_FULLFSYNC on macOS — see e2e_syncfail.sh, which runs on Linux only),
 * so failing fsync() drives closePartFile(true) to return false while the .part
 * payload still sits in the page cache and the checksum re-read passes anyway —
 * exactly the case the fix must catch.
 *
 * Loaded around the receiver under test only, via LD_PRELOAD. Written in C so the
 * sanitizer CI job (which overrides CMAKE_CXX_FLAGS only) leaves it uninstrumented
 * and it preloads cleanly ahead of the ASan runtime. injected_fsync() forwards
 * nothing, so there is no dlsym reentrancy to worry about.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <unistd.h>

static int injected_fsync(int fd) {
    (void)fd;
    errno = EIO;
    return -1;
}

#if defined(__APPLE__)
/* Two-level namespaces ignore a plain override; __interpose remaps callers. */
__attribute__((used, section("__DATA,__interpose")))
static const void* interpose_fsync[2] = {
    (const void*)&injected_fsync,
    (const void*)&fsync,
};
#else
int fsync(int fd) {
    return injected_fsync(fd);
}
#endif
