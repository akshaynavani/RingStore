#pragma once

#include <gtest/gtest.h>
#include <liburing.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

namespace ringstore {
namespace testing {

// Some sandboxes (Docker's default seccomp profile, gVisor, old kernels)
// block io_uring_setup. Detect that once and skip instead of failing.
inline bool UringAvailable() {
  static const bool available = [] {
    io_uring ring;
    if (io_uring_queue_init(4, &ring, 0) != 0) return false;
    io_uring_queue_exit(&ring);
    return true;
  }();
  return available;
}

#define REQUIRE_URING()                                                    \
  do {                                                                     \
    if (!::ringstore::testing::UringAvailable()) {                         \
      GTEST_SKIP() << "io_uring is not available in this environment";    \
    }                                                                      \
  } while (0)

// Unique scratch file removed on destruction.
class TempPath {
 public:
  TempPath() {
    const char* dir = std::getenv("RINGSTORE_TEST_DIR");
    std::string tmpl = std::string(dir != nullptr ? dir : "/tmp") + "/ringstore_test_XXXXXX";
    int fd = ::mkstemp(tmpl.data());
    if (fd >= 0) ::close(fd);
    path_ = tmpl;
  }
  ~TempPath() { ::unlink(path_.c_str()); }

  TempPath(const TempPath&) = delete;
  TempPath& operator=(const TempPath&) = delete;

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

}  // namespace testing
}  // namespace ringstore
