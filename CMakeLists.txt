cmake_minimum_required(VERSION 3.15)
project(filecast CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Version string baked into the binary; the release workflow overrides this
# from the git tag (e.g. -DFILECAST_VERSION=v1.0.0).
set(FILECAST_VERSION "1.0.0" CACHE STRING "Version string for --version output")

# --- main executable ---------------------------------------------------------

add_executable(filecast
    src/Main.cpp
    src/Sender.cpp
    src/Receiver.cpp
    src/Config.cpp
)

target_include_directories(filecast PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/cxxopts/include
)

target_compile_definitions(filecast PRIVATE
    FILECAST_VERSION="${FILECAST_VERSION}"
)

if(MSVC)
    target_compile_options(filecast PRIVATE /W4 /permissive-)
else()
    target_compile_options(filecast PRIVATE -Wall -Wextra)
endif()

if(WIN32)
    target_link_libraries(filecast PRIVATE ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(filecast PRIVATE Threads::Threads)
endif()

# --- tests -------------------------------------------------------------------

option(BUILD_TESTING "Build the unit-test target" ON)

if(BUILD_TESTING)
    enable_testing()
    find_package(GTest QUIET)
    if(GTest_FOUND)
        add_executable(GTests tests/Tests.cpp)
        target_link_libraries(GTests PRIVATE GTest::gtest)
        if(NOT WIN32)
            find_package(Threads REQUIRED)
            target_link_libraries(GTests PRIVATE Threads::Threads)
        endif()
        add_test(NAME unit COMMAND GTests)
    else()
        message(STATUS "GTest not found — skipping GTests target")
    endif()

    # E2E loopback tests (Linux/macOS only — Winsock SO_REUSEADDR semantics
    # break two-process loopback, see tests/e2e.sh comment).
    if(NOT WIN32)
        add_test(
            NAME e2e
            COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e.sh
        )
        set_tests_properties(e2e PROPERTIES
            ENVIRONMENT "BINARY=$<TARGET_FILE:filecast>"
        )

        # Disk-full variant: a tiny C shim (LD_PRELOAD / DYLD_INSERT_LIBRARIES)
        # forces the receiver's first pwrite() to fail with ENOSPC, exercising
        # the write-failure cleanup branch without a real full filesystem. The
        # shim is C so the sanitizer job (which overrides CMAKE_CXX_FLAGS only)
        # leaves it uninstrumented and it preloads cleanly into the receiver.
        enable_language(C)
        add_library(failpwrite SHARED tests/failpwrite.c)
        target_link_libraries(failpwrite PRIVATE ${CMAKE_DL_LIBS})

        add_test(
            NAME e2e_diskfull
            COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_diskfull.sh
        )
        set_tests_properties(e2e_diskfull PROPERTIES
            ENVIRONMENT "BINARY=$<TARGET_FILE:filecast>;FAILLIB=$<TARGET_FILE:failpwrite>"
            TIMEOUT 120
        )

        # Durable-sync-failure variant: a sibling C shim forces the receiver's
        # fsync() to fail with EIO, so a completed transfer's durable sync fails
        # while the page-cache re-read still passes — pinning that verifyAndWrite
        # never reports "sha256 verified" for un-synced data. Linux only (the
        # e2e_syncfail.sh script self-skips elsewhere; see its header).
        add_library(failsync SHARED tests/failsync.c)

        add_test(
            NAME e2e_syncfail
            COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_syncfail.sh
        )
        set_tests_properties(e2e_syncfail PROPERTIES
            ENVIRONMENT "BINARY=$<TARGET_FILE:filecast>;FAILLIB=$<TARGET_FILE:failsync>"
            TIMEOUT 120
        )

        # Send-failure variant: a sibling C shim forces the sender's sendto() to
        # fail with ENETDOWN, so a total local send failure (network unreachable)
        # exits non-zero instead of printing "Sent ..." and returning 0.
        add_library(failsendto SHARED tests/failsendto.c)
        target_link_libraries(failsendto PRIVATE ${CMAKE_DL_LIBS})

        add_test(
            NAME e2e_sendfail
            COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_sendfail.sh
        )
        set_tests_properties(e2e_sendfail PROPERTIES
            ENVIRONMENT "BINARY=$<TARGET_FILE:filecast>;FAILLIB=$<TARGET_FILE:failsendto>"
            TIMEOUT 120
        )

        # Lossy variant: a Python UDP proxy drops packets in both directions to
        # exercise the RESEND recovery branch. Skipped if Python 3 is missing.
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(
                NAME e2e_lossy
                COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_lossy.sh
            )
            set_tests_properties(e2e_lossy PROPERTIES
                ENVIRONMENT "BINARY=$<TARGET_FILE:filecast>;PYTHON=${Python3_EXECUTABLE}"
                TIMEOUT 120
            )

            # Corrupting variant: a Python UDP proxy flips one payload byte so the
            # reassembled file fails its SHA-256, exercising verifyAndWrite's
            # checksum-mismatch cleanup. Also needs Python 3.
            add_test(
                NAME e2e_corrupt
                COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_corrupt.sh
            )
            set_tests_properties(e2e_corrupt PROPERTIES
                ENVIRONMENT "BINARY=$<TARGET_FILE:filecast>;PYTHON=${Python3_EXECUTABLE}"
                TIMEOUT 120
            )

            # Session-hijack variant: a Python UDP proxy injects one forged ANNOUNCE
            # reusing the legitimate session id but claiming a different file, so a
            # receiver that re-latched it would reset/hijack the transfer. The script
            # runs both a valid-conflict and an invalid (out-of-range chunk) forgery
            # to pin the same-session ANNOUNCE guard in handleAnnounce. Needs Python 3.
            add_test(
                NAME e2e_hijack
                COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_hijack.sh
            )
            set_tests_properties(e2e_hijack PROPERTIES
                ENVIRONMENT "BINARY=$<TARGET_FILE:filecast>;PYTHON=${Python3_EXECUTABLE}"
                TIMEOUT 120
            )

            # Flood variant: a Python UDP flooder keeps the socket busy so
            # recvfrom never reports an idle read, pinning that the receiver's
            # wait and the sender's resend phase now time out on a wall-clock
            # deadline instead of being kept alive forever. Also needs Python 3.
            add_test(
                NAME e2e_flood
                COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_flood.sh
            )
            set_tests_properties(e2e_flood PROPERTIES
                ENVIRONMENT "BINARY=$<TARGET_FILE:filecast>;PYTHON=${Python3_EXECUTABLE}"
                TIMEOUT 120
            )
        else()
            message(STATUS "Python 3 not found — skipping e2e_lossy, e2e_corrupt, e2e_hijack and e2e_flood tests")
        endif()
    endif()
endif()
