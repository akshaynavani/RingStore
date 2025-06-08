#pragma once

#include <liburing.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include "ringstore/status.h"

namespace ringstore {

struct RingOptions {
  unsigned entries = 256;          // SQ depth (rounded up to a power of two by the kernel)
  bool sqpoll = false;             // kernel-side submission polling thread
  unsigned sqpoll_idle_ms = 2000;  // idle time before the SQPOLL thread sleeps
  bool iopoll = false;             // busy-poll completions (requires O_DIRECT + polled device)
  unsigned cq_entries = 0;         // 0 => kernel default (2 * entries)
};

// Thin RAII wrapper around `struct io_uring`. Not thread-safe; callers that
// share a Ring across threads must synchronise externally (IoEngine does).
class Ring {
 public:
  Ring() = default;
  ~Ring();

  Ring(const Ring&) = delete;
  Ring& operator=(const Ring&) = delete;

  Status Init(const RingOptions& options);
  bool initialized() const { return initialized_; }
  const RingOptions& options() const { return options_; }

  // Returns nullptr if the submission queue is full.
  io_uring_sqe* GetSqe();
  unsigned SqSpaceLeft();

  // Submits all queued SQEs. Returns number submitted or a negative errno.
  int Submit();
  // Submits and waits for at least `wait_nr` completions.
  int SubmitAndWait(unsigned wait_nr);

  // Blocks until a CQE is available. Returns negative errno on failure.
  int WaitCqe(io_uring_cqe** cqe);
  // Waits with a timeout. Returns -ETIME on timeout.
  int WaitCqeTimeout(io_uring_cqe** cqe, std::chrono::microseconds timeout);
  // Non-blocking. Returns -EAGAIN if nothing is ready.
  int PeekCqe(io_uring_cqe** cqe);
  // Fills up to `count` CQEs without blocking; returns number filled.
  unsigned PeekBatch(io_uring_cqe** cqes, unsigned count);
  void CqeSeen(io_uring_cqe* cqe);
  void CqAdvance(unsigned n);

  Status RegisterBuffers(const std::vector<iovec>& iovecs);
  Status UnregisterBuffers();
  Status RegisterFiles(const std::vector<int>& fds);
  Status UnregisterFiles();

  bool buffers_registered() const { return buffers_registered_; }
  bool files_registered() const { return files_registered_; }

  // Feature probe: whether the running kernel supports a given opcode.
  bool SupportsOp(int opcode) const;

  io_uring* raw() { return &ring_; }

 private:
  io_uring ring_{};
  RingOptions options_{};
  bool initialized_ = false;
  bool buffers_registered_ = false;
  bool files_registered_ = false;
  io_uring_probe* probe_ = nullptr;
};

}  // namespace ringstore
