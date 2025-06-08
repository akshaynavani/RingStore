#pragma once

#include <cstdint>
#include <string>

#include "ringstore/status.h"

namespace ringstore {

struct FileOptions {
  bool create = false;     // O_CREAT
  bool truncate = false;   // O_TRUNC
  bool direct = false;     // O_DIRECT: bypass page cache (requires aligned I/O)
  bool read_only = false;  // O_RDONLY instead of O_RDWR
  bool dsync = false;      // O_DSYNC: each write is durable on completion
  unsigned mode = 0644;
};

// RAII file descriptor wrapper usable for both regular files and raw block
// devices (e.g. /dev/nvme0n1). Size and logical block size are resolved via
// ioctl for block devices and fstat for regular files.
class File {
 public:
  File() = default;
  ~File();

  File(const File&) = delete;
  File& operator=(const File&) = delete;
  File(File&& other) noexcept;
  File& operator=(File&& other) noexcept;

  static Status Open(const std::string& path, const FileOptions& options, File* out);

  int fd() const { return fd_; }
  bool is_open() const { return fd_ >= 0; }
  bool is_block_device() const { return is_block_device_; }
  bool is_direct() const { return direct_; }
  const std::string& path() const { return path_; }

  Status Size(std::uint64_t* size) const;
  // Logical sector size for block devices; 512 for regular files.
  std::uint32_t logical_block_size() const { return logical_block_size_; }

  // Ensures the file is at least `size` bytes (no-op for block devices).
  Status Allocate(std::uint64_t size);
  Status Sync();
  Status Close();

 private:
  int fd_ = -1;
  bool is_block_device_ = false;
  bool direct_ = false;
  std::uint32_t logical_block_size_ = 512;
  std::string path_;
};

}  // namespace ringstore
