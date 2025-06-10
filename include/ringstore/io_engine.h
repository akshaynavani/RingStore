#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "ringstore/engine_options.h"
#include "ringstore/io_request.h"
#include "ringstore/ring.h"
#include "ringstore/status.h"

namespace ringstore {

// Asynchronous I/O engine on top of a single io_uring instance.
//
// * Any thread may submit. Submission is serialised by a mutex and applies
//   back-pressure once `max_inflight` requests are outstanding.
// * A dedicated reaper thread drains the completion queue, transparently
//   resubmits short reads/writes and EAGAIN, and then invokes the request's
//   callback.
// * Shutdown() stops accepting new work, drains everything in flight and joins
//   the reaper thread.
class IoEngine {
 public:
  IoEngine() = default;
  ~IoEngine();

  IoEngine(const IoEngine&) = delete;
  IoEngine& operator=(const IoEngine&) = delete;

  Status Start(const EngineOptions& options = EngineOptions{});
  void Shutdown();
  bool running() const { return running_.load(std::memory_order_acquire); }

  // Queues and submits a single request. Blocks while the engine is at
  // `max_inflight` (unless called from the reaper thread, in which case it
  // returns ResourceExhausted instead of deadlocking).
  Status Submit(IoRequest request);

  // Queues all requests and issues as few io_uring_enter calls as possible.
  // Either every request is accepted or an error is returned before any
  // callback could fire for the rejected tail.
  Status SubmitBatch(std::vector<IoRequest> requests);

  EngineStats stats() const;
  unsigned max_inflight() const { return max_inflight_; }
  Ring& ring() { return ring_; }

 private:
  struct Inflight {
    IoRequest req;
    std::uint32_t done = 0;  // bytes completed so far
    std::uint32_t resubmits = 0;
    std::uint64_t start_ns = 0;
  };

  Status ValidateLocked(const IoRequest& req) const;
  io_uring_sqe* AcquireSqeLocked();
  void PrepareSqe(io_uring_sqe* sqe, Inflight* inflight);
  int FlushLocked();
  bool OnReaperThread() const;

  void ReaperLoop();
  void HandleCompletion(Inflight* inflight, int res);
  void Resubmit(Inflight* inflight);
  void Complete(Inflight* inflight, Status status);

  Ring ring_;
  EngineOptions options_;
  unsigned max_inflight_ = 0;

  mutable std::mutex mu_;
  std::condition_variable space_cv_;   // signalled when inflight drops
  std::condition_variable drained_cv_; // signalled when inflight reaches 0
  unsigned inflight_ = 0;
  unsigned unsubmitted_ = 0;           // SQEs prepared but not yet submitted
  bool accepting_ = false;

  std::atomic<bool> running_{false};
  std::thread reaper_;
  std::thread::id reaper_id_;

  // Stats (atomics so stats() never contends with the hot path for long).
  std::atomic<std::uint64_t> submitted_{0};
  std::atomic<std::uint64_t> completed_{0};
  std::atomic<std::uint64_t> errors_{0};
  std::atomic<std::uint64_t> resubmits_{0};
  std::atomic<std::uint64_t> bytes_read_{0};
  std::atomic<std::uint64_t> bytes_written_{0};
  std::atomic<std::uint64_t> submit_syscalls_{0};
  std::atomic<std::uint64_t> high_water_{0};
};

}  // namespace ringstore
