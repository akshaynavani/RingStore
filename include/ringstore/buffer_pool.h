#pragma once

#include <sys/uio.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "ringstore/aligned_buffer.h"

namespace ringstore {

class BufferPool;

// A borrowed buffer slot from a BufferPool. Returns itself to the pool on
// destruction. `index()` doubles as the io_uring fixed-buffer index when the
// pool has been registered with a Ring.
class PooledBuffer {
 public:
  PooledBuffer() = default;
  ~PooledBuffer() { Reset(); }

  PooledBuffer(const PooledBuffer&) = delete;
  PooledBuffer& operator=(const PooledBuffer&) = delete;
  PooledBuffer(PooledBuffer&& other) noexcept { *this = std::move(other); }
  PooledBuffer& operator=(PooledBuffer&& other) noexcept;

  std::uint8_t* data() const { return data_; }
  std::size_t size() const { return size_; }
  int index() const { return index_; }
  bool valid() const { return pool_ != nullptr; }

  void Reset();

 private:
  friend class BufferPool;
  PooledBuffer(BufferPool* pool, int index, std::uint8_t* data, std::size_t size)
      : pool_(pool), index_(index), data_(data), size_(size) {}

  BufferPool* pool_ = nullptr;
  int index_ = -1;
  std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
};

// Fixed-capacity pool of equally sized, aligned buffers. All buffers live in
// one contiguous allocation so they can be registered with io_uring in a
// single call (IORING_REGISTER_BUFFERS) and used with READ_FIXED/WRITE_FIXED.
class BufferPool {
 public:
  BufferPool(std::size_t buffer_count, std::size_t buffer_size,
             std::size_t alignment = kDefaultAlignment);

  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;

  // Blocks until a buffer is available.
  PooledBuffer Acquire();
  // Returns std::nullopt if no buffer becomes available within `timeout`.
  std::optional<PooledBuffer> TryAcquire(
      std::chrono::milliseconds timeout = std::chrono::milliseconds(0));

  std::size_t buffer_size() const { return buffer_size_; }
  std::size_t capacity() const { return iovecs_.size(); }
  std::size_t available() const;

  // iovec array describing every buffer, index-aligned with PooledBuffer::index().
  const std::vector<iovec>& iovecs() const { return iovecs_; }

 private:
  friend class PooledBuffer;
  void Release(int index);
  PooledBuffer MakeHandle(int index);

  std::size_t buffer_size_;
  AlignedBuffer arena_;
  std::vector<iovec> iovecs_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::vector<int> free_list_;
};

}  // namespace ringstore
