cmake_minimum_required(VERSION 3.16)
project(RingStore VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(RINGSTORE_WERROR "Treat compiler warnings as errors" OFF)
option(RINGSTORE_BUILD_TESTS "Build Google Test suites" ON)
option(RINGSTORE_BUILD_BENCH "Build the benchmark tool" ON)
option(RINGSTORE_SANITIZE "Build with address + undefined sanitizers" OFF)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "RingStore requires Linux (io_uring). Detected: ${CMAKE_SYSTEM_NAME}")
endif()

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(URING REQUIRED IMPORTED_TARGET liburing>=2.0)

set(RINGSTORE_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(RINGSTORE_WERROR)
  list(APPEND RINGSTORE_WARNINGS -Werror)
endif()

add_library(ringstore STATIC
  src/status.cpp
  src/aligned_buffer.cpp
  src/buffer_pool.cpp
  src/ring.cpp
  src/file.cpp
  src/io_engine.cpp
)

target_include_directories(ringstore PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(ringstore PUBLIC PkgConfig::URING Threads::Threads)
target_compile_options(ringstore PRIVATE ${RINGSTORE_WARNINGS})

if(RINGSTORE_SANITIZE)
  target_compile_options(ringstore PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(ringstore PUBLIC -fsanitize=address,undefined)
endif()

add_executable(ringstore_example examples/basic_io.cpp)
target_link_libraries(ringstore_example PRIVATE ringstore)
target_compile_options(ringstore_example PRIVATE ${RINGSTORE_WARNINGS})
