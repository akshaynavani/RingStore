include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
  URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(ringstore_tests
  test_buffer.cpp
  test_ring.cpp
  test_engine.cpp
  test_crc32c.cpp
  test_block_store.cpp
  test_histogram.cpp
)
target_include_directories(ringstore_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/bench
)
target_link_libraries(ringstore_tests PRIVATE ringstore GTest::gtest_main)

# Tests create scratch files under $RINGSTORE_TEST_DIR (default: /tmp).
gtest_discover_tests(ringstore_tests DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 120)
