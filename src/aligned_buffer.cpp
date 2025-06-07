#include "ringstore/aligned_buffer.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>

namespace ringstore {

namespace {
bool IsPowerOfTwo(std::size_t v) { return v != 0 && (v & (v - 1)) == 0; }
}  // namespace

AlignedBuffer::AlignedBuffer(std::size_t size, std::size_t alignment)
    : size_(size), alignment_(alignment) {
  if (!IsPowerOfTwo(alignment) || alignment < sizeof(void*)) {
    throw std::invalid_argument("AlignedBuffer: alignment must be a power of two >= sizeof(void*)");
  }
  if (size == 0) return;
  // Round the allocation up so the whole buffer can be used for O_DIRECT I/O.
  std::size_t alloc = (size + alignment - 1) & ~(alignment - 1);
  void* p = nullptr;
  if (posix_memalign(&p, alignment, alloc) != 0) {
    throw std::bad_alloc();
  }
  std::memset(p, 0, alloc);
  data_ = static_cast<std::uint8_t*>(p);
}

AlignedBuffer::~AlignedBuffer() { Release(); }

AlignedBuffer::AlignedBuffer(AlignedBuffer&& other) noexcept
    : data_(other.data_), size_(other.size_), alignment_(other.alignment_) {
  other.data_ = nullptr;
  other.size_ = 0;
}

AlignedBuffer& AlignedBuffer::operator=(AlignedBuffer&& other) noexcept {
  if (this != &other) {
    Release();
    data_ = other.data_;
    size_ = other.size_;
    alignment_ = other.alignment_;
    other.data_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

void AlignedBuffer::Fill(std::uint8_t value) {
  if (data_) std::memset(data_, value, size_);
}

void AlignedBuffer::Release() {
  std::free(data_);
  data_ = nullptr;
  size_ = 0;
}

}  // namespace ringstore
