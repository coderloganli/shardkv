cmake_minimum_required(VERSION 3.22)
project(shardkv CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(cmake/Sanitizers.cmake)

add_compile_options(-Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------- the library

add_library(shardkv_lib
  src/base/buffer.cc
  src/proto/parser.cc
  src/proto/encoder.cc
  src/store/shard.cc
  src/commands/dispatch.cc
  src/net/listener.cc
  src/net/connection.cc
  src/net/loop.cc
)
target_include_directories(shardkv_lib PUBLIC src third_party)
shardkv_apply_sanitizers(shardkv_lib)

# ----------------------------------------------------------------- the server

add_executable(shardkv src/main.cc)
target_link_libraries(shardkv PRIVATE shardkv_lib)
shardkv_apply_sanitizers(shardkv)

# ------------------------------------------------------------------ the tests

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)

# ThreadSanitizer needs the address space laid out where it expects. Kernels
# from Ubuntu 24.04 onward randomise mmap with 32 bits of entropy by default,
# which TSan rejects outright:
#
#   FATAL: ThreadSanitizer: unexpected memory mapping ...
#
# Running the binaries under `setarch -R` turns randomisation off for those
# processes, which is narrower than changing vm.mmap_rnd_bits for the whole
# machine. Note that the personality() syscall this uses is blocked by Docker's
# default seccomp profile, so the container needs
# --security-opt seccomp=unconfined for the thread build. Both are recorded in
# docs/architecture.md rather than left as folklore.
set(SHARDKV_TEST_LAUNCHER "")
if(SHARDKV_SANITIZER STREQUAL "thread")
  find_program(SETARCH_EXECUTABLE setarch REQUIRED)
  set(SHARDKV_TEST_LAUNCHER ${SETARCH_EXECUTABLE} ${CMAKE_SYSTEM_PROCESSOR} -R)
endif()

# One binary per area, so a failure names the area before it names the case.
foreach(area parser encoder store expiry dispatch unique_fd integration)
  add_executable(${area}_test tests/${area}_test.cc)
  target_link_libraries(${area}_test PRIVATE shardkv_lib GTest::gtest_main)
  target_include_directories(${area}_test PRIVATE tests)
  shardkv_apply_sanitizers(${area}_test)
  if(SHARDKV_TEST_LAUNCHER)
    # CROSSCOMPILING_EMULATOR rather than gtest_discover_tests(LAUNCHER ...):
    # discovery runs the binary too, to enumerate the cases, and LAUNCHER does
    # not apply to that run -- so discovery would hit the TSan mapping error
    # before a single test executed. The emulator property is used for both.
    set_property(TARGET ${area}_test PROPERTY CROSSCOMPILING_EMULATOR ${SHARDKV_TEST_LAUNCHER})
  endif()
  gtest_discover_tests(${area}_test)
endforeach()
