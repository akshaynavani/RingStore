#pragma once

#include <cstdint>
#include <functional>

#include "ringstore/status.h"

namespace ringstore {

enum class IoOp : std::uint8_t {
  kRead,
  kWrite,
  kFsync,      // full fsync
  kFdatasync,  // fsync with IORING_FSYNC_DATASYNC
  kNop,
};

const char* IoOpName(IoOp op);

struct IoResult {
  Status status;
  // Total bytes transferred (after any short-I/O resubmissions).
  std::uint32_t bytes = 0;
  // Submit-to-completion latency including any resubmissions.
  std::uint64_t latency_ns = 0;
  // Number of times the request had to be resubmitted (short I/O / EAGAIN).
  std::uint32_t resubmits = 0;
};

// Invoked exactly once per accepted request, on the engine's reaper thread.
// Callbacks must be short and must not block waiting on other I/O.
using IoCallback = std::function<void(const IoResult&)>;

struct IoRequest {
  IoOp op = IoOp::kNop;
  // A raw file descriptor, or an index into the registered file table when
  // `fixed_file` is set.
  int fd = -1;
  bool fixed_file = false;
  std::uint64_t offset = 0;
  std::uint8_t* buf = nullptr;
  std::uint32_t len = 0;
  // >= 0 selects READ_FIXED / WRITE_FIXED against a registered buffer.
  int buf_index = -1;
  IoCallback callback;

  static IoRequest Read(int fd, std::uint8_t* buf, std::uint32_t len, std::uint64_t offset,
                        IoCallback cb = nullptr) {
    IoRequest r;
    r.op = IoOp::kRead;
    r.fd = fd;
    r.buf = buf;
    r.len = len;
    r.offset = offset;
    r.callback = std::move(cb);
    return r;
  }

  static IoRequest Write(int fd, std::uint8_t* buf, std::uint32_t len, std::uint64_t offset,
                         IoCallback cb = nullptr) {
    IoRequest r;
    r.op = IoOp::kWrite;
    r.fd = fd;
    r.buf = buf;
    r.len = len;
    r.offset = offset;
    r.callback = std::move(cb);
    return r;
  }

  static IoRequest Fsync(int fd, bool datasync = false, IoCallback cb = nullptr) {
    IoRequest r;
    r.op = datasync ? IoOp::kFdatasync : IoOp::kFsync;
    r.fd = fd;
    r.callback = std::move(cb);
    return r;
  }
};

}  // namespace ringstore
