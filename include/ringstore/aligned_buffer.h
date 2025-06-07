#pragma once

#include <cstddef>
#include <cstdint>

namespace ringstore {

constexpr std::size_t kDefaultAlignment = 4096;

// Owning, move-only buffer with a guaranteed memory alignment. O_DIRECT I/O
// requires the user buffer, file offset and length to be aligned to the
// logical block size of the device, so every data-path buffer uses this type.
class AlignedBuffer {
 public:
  AlignedBuffer() = default;
  explicit AlignedBuffer(std::size_t size, std::size_t alignment = kDefaultAlignment);
  ~AlignedBuffer();

  AlignedBuffer(const AlignedBuffer&) = delete;
  AlignedBuffer& operator=(const AlignedBuffer&) = delete;
  AlignedBuffer(AlignedBuffer&& other) noexcept;
  AlignedBuffer& operator=(AlignedBuffer&& other) noexcept;

  std::uint8_t* data() { return data_; }
  const std::uint8_t* data() const { return data_; }
  std::size_t size() const { return size_; }
  std::size_t alignment() const { return alignment_; }
  bool empty() const { return data_ == nullptr; }

  void Fill(std::uint8_t value);

 private:
  void Release();

  std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t alignment_ = kDefaultAlignment;
};

}  // namespace ringstore
