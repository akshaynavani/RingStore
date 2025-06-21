#include "ringstore/io_engine.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <memory>
#include <string>

namespace ringstore {

namespace {

// user_data value used to wake the reaper thread during shutdown.
char kShutdownSentinel;

std::uint64_t NowNs() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

bool IsDataOp(IoOp op) { return op == IoOp::kRead || op == IoOp::kWrite; }

constexpr unsigned kReapBatch = 64;

}  // namespace

const char* IoOpName(IoOp op) {
  switch (op) {
    case IoOp::kRead: return "read";
    case IoOp::kWrite: return "write";
    case IoOp::kFsync: return "fsync";
    case IoOp::kFdatasync: return "fdatasync";
    case IoOp::kNop: return "nop";
  }
  return "unknown";
}

IoEngine::~IoEngine() { Shutdown(); }

Status IoEngine::Start(const EngineOptions& options) {
  std::lock_guard<std::mutex> lock(mu_);
  if (running_.load()) return Status::InvalidArgument("engine already running");

  options_ = options;
  Status s = ring_.Init(options.ring);
  if (!s.ok()) return s;

  unsigned sq_entries = ring_.raw()->sq.ring_entries;
  max_inflight_ = options.max_inflight == 0 ? sq_entries : std::min(options.max_inflight, sq_entries);

  accepting_ = true;
  running_.store(true, std::memory_order_release);
  reaper_ = std::thread([this] { ReaperLoop(); });
  reaper_id_ = reaper_.get_id();
  return Status::Ok();
}

void IoEngine::Shutdown() {
  {
    std::unique_lock<std::mutex> lock(mu_);
    if (!running_.load()) return;
    if (OnReaperThread()) {
      // Shutting down from a completion callback would self-deadlock on join.
      accepting_ = false;
      return;
    }
    accepting_ = false;
    space_cv_.notify_all();
    drained_cv_.wait(lock, [this] { return inflight_ == 0; });

    io_uring_sqe* sqe = AcquireSqeLocked();
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data(sqe, &kShutdownSentinel);
    ++unsubmitted_;
    FlushLocked();
  }
  if (reaper_.joinable()) reaper_.join();
  running_.store(false, std::memory_order_release);
}

bool IoEngine::OnReaperThread() const { return std::this_thread::get_id() == reaper_id_; }

Status IoEngine::ValidateLocked(const IoRequest& req) const {
  if (req.op != IoOp::kNop && req.fd < 0) return Status::InvalidArgument("invalid fd");
  if (IsDataOp(req.op)) {
    if (req.buf == nullptr || req.len == 0) {
      return Status::InvalidArgument("read/write requires a non-empty buffer");
    }
  }
  if (req.buf_index >= 0) {
    if (!IsDataOp(req.op)) return Status::InvalidArgument("buf_index only valid for read/write");
    if (!ring_.buffers_registered()) return Status::InvalidArgument("no registered buffers");
  }
  if (req.fixed_file && !ring_.files_registered()) {
    return Status::InvalidArgument("no registered files");
  }
  return Status::Ok();
}

io_uring_sqe* IoEngine::AcquireSqeLocked() {
  for (;;) {
    io_uring_sqe* sqe = ring_.GetSqe();
    if (sqe != nullptr) return sqe;
    // SQ full: push what we have to the kernel. Under SQPOLL the kernel thread
    // may still be consuming entries, so spin politely until a slot frees up.
    FlushLocked();
    std::this_thread::yield();
  }
}

void IoEngine::PrepareSqe(io_uring_sqe* sqe, Inflight* inflight) {
  const IoRequest& r = inflight->req;
  std::uint8_t* buf = r.buf + inflight->done;
  unsigned len = r.len - inflight->done;
  std::uint64_t off = r.offset + inflight->done;

  switch (r.op) {
    case IoOp::kRead:
      if (r.buf_index >= 0) {
        io_uring_prep_read_fixed(sqe, r.fd, buf, len, off, r.buf_index);
      } else {
        io_uring_prep_read(sqe, r.fd, buf, len, off);
      }
      break;
    case IoOp::kWrite:
      if (r.buf_index >= 0) {
        io_uring_prep_write_fixed(sqe, r.fd, buf, len, off, r.buf_index);
      } else {
        io_uring_prep_write(sqe, r.fd, buf, len, off);
      }
      break;
    case IoOp::kFsync:
      io_uring_prep_fsync(sqe, r.fd, 0);
      break;
    case IoOp::kFdatasync:
      io_uring_prep_fsync(sqe, r.fd, IORING_FSYNC_DATASYNC);
      break;
    case IoOp::kNop:
      io_uring_prep_nop(sqe);
      break;
  }
  if (r.fixed_file && r.op != IoOp::kNop) {
    io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
  }
  io_uring_sqe_set_data(sqe, inflight);
}

int IoEngine::FlushLocked() {
  if (unsubmitted_ == 0) return 0;
  int ret = ring_.Submit();
  submit_syscalls_.fetch_add(1, std::memory_order_relaxed);
  if (ret > 0) {
    unsubmitted_ -= std::min(static_cast<unsigned>(ret), unsubmitted_);
  }
  // On -EBUSY / -EAGAIN the SQEs stay queued; the reaper retries the flush
  // after draining completions, so nothing is stranded.
  return ret;
}

Status IoEngine::Submit(IoRequest request) {
  std::unique_lock<std::mutex> lock(mu_);
  if (!accepting_) return Status::Shutdown("engine is not accepting requests");
  Status s = ValidateLocked(request);
  if (!s.ok()) return s;

  if (OnReaperThread()) {
    if (inflight_ >= max_inflight_) {
      return Status::ResourceExhausted("engine at max_inflight (submit from callback)");
    }
  } else {
    space_cv_.wait(lock, [this] { return inflight_ < max_inflight_ || !accepting_; });
    if (!accepting_) return Status::Shutdown("engine is shutting down");
  }

  auto* inflight = new Inflight{std::move(request), 0, 0, NowNs()};
  io_uring_sqe* sqe = AcquireSqeLocked();
  PrepareSqe(sqe, inflight);
  ++inflight_;
  ++unsubmitted_;
  submitted_.fetch_add(1, std::memory_order_relaxed);
  if (inflight_ > high_water_.load(std::memory_order_relaxed)) {
    high_water_.store(inflight_, std::memory_order_relaxed);
  }
  // Requests chained from a completion callback are left queued: the reaper
  // flushes once per drained CQ batch, so a closed loop at queue depth N costs
  // one io_uring_enter per batch instead of one per request.
  if (!OnReaperThread()) FlushLocked();
  return Status::Ok();
}

Status IoEngine::SubmitBatch(std::vector<IoRequest> requests) {
  if (requests.empty()) return Status::Ok();
  std::unique_lock<std::mutex> lock(mu_);
  if (!accepting_) return Status::Shutdown("engine is not accepting requests");
  for (const auto& r : requests) {
    Status s = ValidateLocked(r);
    if (!s.ok()) return s;
  }
  const bool on_reaper = OnReaperThread();
  if (on_reaper && inflight_ + requests.size() > max_inflight_) {
    return Status::ResourceExhausted("batch exceeds max_inflight (submit from callback)");
  }

  for (std::size_t i = 0; i < requests.size(); ++i) {
    if (inflight_ >= max_inflight_) {
      // Make sure everything already prepared is visible to the kernel before
      // sleeping, otherwise nothing could complete to free up space.
      FlushLocked();
      space_cv_.wait(lock, [this] { return inflight_ < max_inflight_ || !accepting_; });
      if (!accepting_) {
        FlushLocked();
        return Status::Shutdown("engine shut down mid-batch; " + std::to_string(i) +
                                " of " + std::to_string(requests.size()) + " accepted");
      }
    }
    auto* inflight = new Inflight{std::move(requests[i]), 0, 0, NowNs()};
    io_uring_sqe* sqe = AcquireSqeLocked();
    PrepareSqe(sqe, inflight);
    ++inflight_;
    ++unsubmitted_;
    submitted_.fetch_add(1, std::memory_order_relaxed);
  }
  if (inflight_ > high_water_.load(std::memory_order_relaxed)) {
    high_water_.store(inflight_, std::memory_order_relaxed);
  }
  if (!on_reaper) FlushLocked();
  return Status::Ok();
}

void IoEngine::ReaperLoop() {
  io_uring_cqe* cqes[kReapBatch];
  bool stop = false;
  while (!stop) {
    io_uring_cqe* first = nullptr;
    int ret = ring_.WaitCqe(&first);
    if (ret < 0) {
      if (ret == -EAGAIN || ret == -EINTR) continue;
      // Unrecoverable ring error; nothing sensible left to do but exit.
      break;
    }

    unsigned n = ring_.PeekBatch(cqes, kReapBatch);
    for (unsigned i = 0; i < n; ++i) {
      void* data = io_uring_cqe_get_data(cqes[i]);
      int res = cqes[i]->res;
      if (data == &kShutdownSentinel) {
        stop = true;
        continue;
      }
      HandleCompletion(static_cast<Inflight*>(data), res);
    }
    ring_.CqAdvance(n);

    // Submit everything queued while handling this batch (chained requests
    // and resubmissions), plus any flush that failed earlier with -EBUSY.
    std::lock_guard<std::mutex> lock(mu_);
    if (unsubmitted_ > 0) FlushLocked();
  }
}

void IoEngine::HandleCompletion(Inflight* inflight, int res) {
  const IoRequest& r = inflight->req;
  if (res < 0) {
    if ((res == -EAGAIN || res == -EINTR) && inflight->resubmits < options_.max_resubmits) {
      Resubmit(inflight);
      return;
    }
    Complete(inflight, Status::FromErrno(IoOpName(r.op), -res));
    return;
  }

  if (IsDataOp(r.op)) {
    inflight->done += static_cast<std::uint32_t>(res);
    if (inflight->done < r.len) {
      if (res == 0) {
        // Read hit EOF: report the short transfer. A zero-byte write means no
        // forward progress is possible (e.g. device full).
        if (r.op == IoOp::kRead) {
          Complete(inflight, Status::Ok());
        } else {
          Complete(inflight, Status::IoError("write made no progress", ENOSPC));
        }
        return;
      }
      if (inflight->resubmits >= options_.max_resubmits) {
        Complete(inflight, Status::IoError("too many short transfers"));
        return;
      }
      Resubmit(inflight);
      return;
    }
  }
  Complete(inflight, Status::Ok());
}

void IoEngine::Resubmit(Inflight* inflight) {
  ++inflight->resubmits;
  resubmits_.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(mu_);
  io_uring_sqe* sqe = AcquireSqeLocked();
  PrepareSqe(sqe, inflight);
  ++unsubmitted_;
  // Runs on the reaper thread; flushed with the rest of the batch.
}

void IoEngine::Complete(Inflight* inflight, Status status) {
  IoResult result;
  result.bytes = inflight->done;
  result.latency_ns = NowNs() - inflight->start_ns;
  result.resubmits = inflight->resubmits;
  result.status = std::move(status);

  completed_.fetch_add(1, std::memory_order_relaxed);
  if (!result.status.ok()) {
    errors_.fetch_add(1, std::memory_order_relaxed);
  } else if (inflight->req.op == IoOp::kRead) {
    bytes_read_.fetch_add(result.bytes, std::memory_order_relaxed);
  } else if (inflight->req.op == IoOp::kWrite) {
    bytes_written_.fetch_add(result.bytes, std::memory_order_relaxed);
  }

  IoCallback cb = std::move(inflight->req.callback);
  delete inflight;

  // Release the slot before running the callback so a callback can chain a
  // follow-up request (closed-loop benchmarks rely on this).
  {
    std::lock_guard<std::mutex> lock(mu_);
    --inflight_;
    if (inflight_ == 0) drained_cv_.notify_all();
  }
  space_cv_.notify_one();

  if (cb) cb(result);
}

Status IoEngine::RegisterBufferPool(const BufferPool& pool) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!running_.load()) return Status::InvalidArgument("engine not started");
  if (inflight_ != 0) return Status::InvalidArgument("cannot register buffers with I/O in flight");
  return ring_.RegisterBuffers(pool.iovecs());
}

Status IoEngine::UnregisterBuffers() {
  std::lock_guard<std::mutex> lock(mu_);
  if (inflight_ != 0) return Status::InvalidArgument("cannot unregister buffers with I/O in flight");
  return ring_.UnregisterBuffers();
}

Status IoEngine::RegisterFiles(const std::vector<int>& fds) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!running_.load()) return Status::InvalidArgument("engine not started");
  if (inflight_ != 0) return Status::InvalidArgument("cannot register files with I/O in flight");
  return ring_.RegisterFiles(fds);
}

Status IoEngine::UnregisterFiles() {
  std::lock_guard<std::mutex> lock(mu_);
  if (inflight_ != 0) return Status::InvalidArgument("cannot unregister files with I/O in flight");
  return ring_.UnregisterFiles();
}

std::future<IoResult> IoEngine::SubmitAsync(IoRequest request) {
  auto promise = std::make_shared<std::promise<IoResult>>();
  std::future<IoResult> future = promise->get_future();
  IoCallback user_cb = std::move(request.callback);
  request.callback = [promise, user_cb = std::move(user_cb)](const IoResult& r) {
    if (user_cb) user_cb(r);
    promise->set_value(r);
  };
  Status s = Submit(std::move(request));
  if (!s.ok()) {
    IoResult failed;
    failed.status = s;
    promise->set_value(failed);
  }
  return future;
}

std::future<IoResult> IoEngine::ReadAsync(int fd, std::uint8_t* buf, std::uint32_t len,
                                          std::uint64_t offset, int buf_index) {
  IoRequest r = IoRequest::Read(fd, buf, len, offset);
  r.buf_index = buf_index;
  return SubmitAsync(std::move(r));
}

std::future<IoResult> IoEngine::WriteAsync(int fd, const std::uint8_t* buf, std::uint32_t len,
                                           std::uint64_t offset, int buf_index) {
  // io_uring never writes into the source buffer; the cast only satisfies the
  // shared IoRequest layout.
  IoRequest r = IoRequest::Write(fd, const_cast<std::uint8_t*>(buf), len, offset);
  r.buf_index = buf_index;
  return SubmitAsync(std::move(r));
}

std::future<IoResult> IoEngine::FsyncAsync(int fd, bool datasync) {
  return SubmitAsync(IoRequest::Fsync(fd, datasync));
}

EngineStats IoEngine::stats() const {
  EngineStats s;
  s.submitted = submitted_.load(std::memory_order_relaxed);
  s.completed = completed_.load(std::memory_order_relaxed);
  s.errors = errors_.load(std::memory_order_relaxed);
  s.resubmits = resubmits_.load(std::memory_order_relaxed);
  s.bytes_read = bytes_read_.load(std::memory_order_relaxed);
  s.bytes_written = bytes_written_.load(std::memory_order_relaxed);
  s.submit_syscalls = submit_syscalls_.load(std::memory_order_relaxed);
  s.inflight_high_water = high_water_.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(mu_);
  s.inflight = inflight_;
  return s;
}

}  // namespace ringstore
