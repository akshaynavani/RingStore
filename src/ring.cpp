#include "ringstore/ring.h"

#include <cerrno>
#include <cstring>

namespace ringstore {

Ring::~Ring() {
  if (!initialized_) return;
  if (buffers_registered_) io_uring_unregister_buffers(&ring_);
  if (files_registered_) io_uring_unregister_files(&ring_);
  if (probe_ != nullptr) io_uring_free_probe(probe_);
  io_uring_queue_exit(&ring_);
}

Status Ring::Init(const RingOptions& options) {
  if (initialized_) return Status::InvalidArgument("Ring already initialized");
  if (options.entries == 0) return Status::InvalidArgument("Ring entries must be > 0");

  options_ = options;
  io_uring_params params;
  std::memset(&params, 0, sizeof(params));
  if (options.sqpoll) {
    params.flags |= IORING_SETUP_SQPOLL;
    params.sq_thread_idle = options.sqpoll_idle_ms;
  }
  if (options.iopoll) params.flags |= IORING_SETUP_IOPOLL;
  if (options.cq_entries != 0) {
    params.flags |= IORING_SETUP_CQSIZE;
    params.cq_entries = options.cq_entries;
  }

  int ret = io_uring_queue_init_params(options.entries, &ring_, &params);
  if (ret < 0) return Status::FromErrno("io_uring_queue_init_params", ret);
  initialized_ = true;
  probe_ = io_uring_get_probe_ring(&ring_);
  return Status::Ok();
}

io_uring_sqe* Ring::GetSqe() { return io_uring_get_sqe(&ring_); }

unsigned Ring::SqSpaceLeft() { return io_uring_sq_space_left(&ring_); }

int Ring::Submit() { return io_uring_submit(&ring_); }

int Ring::SubmitAndWait(unsigned wait_nr) { return io_uring_submit_and_wait(&ring_, wait_nr); }

int Ring::WaitCqe(io_uring_cqe** cqe) {
  int ret;
  do {
    ret = io_uring_wait_cqe(&ring_, cqe);
  } while (ret == -EINTR);
  return ret;
}

int Ring::WaitCqeTimeout(io_uring_cqe** cqe, std::chrono::microseconds timeout) {
  __kernel_timespec ts;
  ts.tv_sec = static_cast<long long>(timeout.count() / 1000000);
  ts.tv_nsec = static_cast<long long>((timeout.count() % 1000000) * 1000);
  int ret;
  do {
    ret = io_uring_wait_cqe_timeout(&ring_, cqe, &ts);
  } while (ret == -EINTR);
  return ret;
}

int Ring::PeekCqe(io_uring_cqe** cqe) { return io_uring_peek_cqe(&ring_, cqe); }

unsigned Ring::PeekBatch(io_uring_cqe** cqes, unsigned count) {
  return io_uring_peek_batch_cqe(&ring_, cqes, count);
}

void Ring::CqeSeen(io_uring_cqe* cqe) { io_uring_cqe_seen(&ring_, cqe); }

void Ring::CqAdvance(unsigned n) { io_uring_cq_advance(&ring_, n); }

Status Ring::RegisterBuffers(const std::vector<iovec>& iovecs) {
  if (buffers_registered_) return Status::InvalidArgument("buffers already registered");
  if (iovecs.empty()) return Status::InvalidArgument("no buffers to register");
  int ret = io_uring_register_buffers(&ring_, iovecs.data(), static_cast<unsigned>(iovecs.size()));
  if (ret < 0) return Status::FromErrno("io_uring_register_buffers", ret);
  buffers_registered_ = true;
  return Status::Ok();
}

Status Ring::UnregisterBuffers() {
  if (!buffers_registered_) return Status::Ok();
  int ret = io_uring_unregister_buffers(&ring_);
  if (ret < 0) return Status::FromErrno("io_uring_unregister_buffers", ret);
  buffers_registered_ = false;
  return Status::Ok();
}

Status Ring::RegisterFiles(const std::vector<int>& fds) {
  if (files_registered_) return Status::InvalidArgument("files already registered");
  if (fds.empty()) return Status::InvalidArgument("no files to register");
  int ret = io_uring_register_files(&ring_, fds.data(), static_cast<unsigned>(fds.size()));
  if (ret < 0) return Status::FromErrno("io_uring_register_files", ret);
  files_registered_ = true;
  return Status::Ok();
}

Status Ring::UnregisterFiles() {
  if (!files_registered_) return Status::Ok();
  int ret = io_uring_unregister_files(&ring_);
  if (ret < 0) return Status::FromErrno("io_uring_unregister_files", ret);
  files_registered_ = false;
  return Status::Ok();
}

bool Ring::SupportsOp(int opcode) const {
  if (probe_ == nullptr) return false;
  return io_uring_opcode_supported(probe_, opcode) != 0;
}

}  // namespace ringstore
