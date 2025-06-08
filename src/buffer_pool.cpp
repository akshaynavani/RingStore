#include "ringstore/buffer_pool.h"

#include <stdexcept>

namespace ringstore {

PooledBuffer& PooledBuffer::operator=(PooledBuffer&& other) noexcept {
  if (this != &other) {
    Reset();
    pool_ = other.pool_;
    index_ = other.index_;
    data_ = other.data_;
    size_ = other.size_;
    other.pool_ = nullptr;
    other.index_ = -1;
    other.data_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

void PooledBuffer::Reset() {
  if (pool_ != nullptr) {
    pool_->Release(index_);
  }
  pool_ = nullptr;
  index_ = -1;
  data_ = nullptr;
  size_ = 0;
}

BufferPool::BufferPool(std::size_t buffer_count, std::size_t buffer_size,
                       std::size_t alignment)
    : buffer_size_(buffer_size) {
  if (buffer_count == 0 || buffer_size == 0) {
    throw std::invalid_argument("BufferPool: count and size must be non-zero");
  }
  if (buffer_size % alignment != 0) {
    throw std::invalid_argument("BufferPool: buffer size must be a multiple of alignment");
  }
  arena_ = AlignedBuffer(buffer_count * buffer_size, alignment);
  iovecs_.reserve(buffer_count);
  free_list_.reserve(buffer_count);
  for (std::size_t i = 0; i < buffer_count; ++i) {
    iovecs_.push_back(iovec{arena_.data() + i * buffer_size, buffer_size});
    // Push in reverse so the lowest index is handed out first.
    free_list_.push_back(static_cast<int>(buffer_count - 1 - i));
  }
}

PooledBuffer BufferPool::MakeHandle(int index) {
  auto& iov = iovecs_[static_cast<std::size_t>(index)];
  return PooledBuffer(this, index, static_cast<std::uint8_t*>(iov.iov_base), iov.iov_len);
}

PooledBuffer BufferPool::Acquire() {
  std::unique_lock<std::mutex> lock(mu_);
  cv_.wait(lock, [this] { return !free_list_.empty(); });
  int idx = free_list_.back();
  free_list_.pop_back();
  return MakeHandle(idx);
}

std::optional<PooledBuffer> BufferPool::TryAcquire(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mu_);
  if (!cv_.wait_for(lock, timeout, [this] { return !free_list_.empty(); })) {
    return std::nullopt;
  }
  int idx = free_list_.back();
  free_list_.pop_back();
  return MakeHandle(idx);
}

std::size_t BufferPool::available() const {
  std::lock_guard<std::mutex> lock(mu_);
  return free_list_.size();
}

void BufferPool::Release(int index) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    free_list_.push_back(index);
  }
  cv_.notify_one();
}

}  // namespace ringstore
