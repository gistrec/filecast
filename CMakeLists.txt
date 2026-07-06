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
        else()
            message(STATUS "Python 3 not found — skipping e2e_lossy and e2e_corrupt tests")
        endif()
    endif()
endif()
