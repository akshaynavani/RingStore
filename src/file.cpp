#include "ringstore/file.h"

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace ringstore {

File::~File() { Close(); }

File::File(File&& other) noexcept { *this = std::move(other); }

File& File::operator=(File&& other) noexcept {
  if (this != &other) {
    Close();
    fd_ = other.fd_;
    is_block_device_ = other.is_block_device_;
    direct_ = other.direct_;
    logical_block_size_ = other.logical_block_size_;
    path_ = std::move(other.path_);
    other.fd_ = -1;
  }
  return *this;
}

Status File::Open(const std::string& path, const FileOptions& options, File* out) {
  int flags = options.read_only ? O_RDONLY : O_RDWR;
  flags |= O_CLOEXEC;
  if (options.create) flags |= O_CREAT;
  if (options.truncate) flags |= O_TRUNC;
  if (options.direct) flags |= O_DIRECT;
  if (options.dsync) flags |= O_DSYNC;

  int fd = ::open(path.c_str(), flags, options.mode);
  if (fd < 0) return Status::FromErrno("open(" + path + ")", errno);

  File f;
  f.fd_ = fd;
  f.path_ = path;
  f.direct_ = options.direct;

  struct stat st;
  if (::fstat(fd, &st) != 0) return Status::FromErrno("fstat(" + path + ")", errno);
  if (S_ISBLK(st.st_mode)) {
    f.is_block_device_ = true;
    int lbs = 0;
    if (::ioctl(fd, BLKSSZGET, &lbs) == 0 && lbs > 0) {
      f.logical_block_size_ = static_cast<std::uint32_t>(lbs);
    }
  }

  *out = std::move(f);
  return Status::Ok();
}

Status File::Size(std::uint64_t* size) const {
  if (fd_ < 0) return Status::InvalidArgument("file not open");
  if (is_block_device_) {
    std::uint64_t bytes = 0;
    if (::ioctl(fd_, BLKGETSIZE64, &bytes) != 0) {
      return Status::FromErrno("ioctl(BLKGETSIZE64)", errno);
    }
    *size = bytes;
    return Status::Ok();
  }
  struct stat st;
  if (::fstat(fd_, &st) != 0) return Status::FromErrno("fstat", errno);
  *size = static_cast<std::uint64_t>(st.st_size);
  return Status::Ok();
}

Status File::Allocate(std::uint64_t size) {
  if (fd_ < 0) return Status::InvalidArgument("file not open");
  if (is_block_device_) {
    std::uint64_t dev_size = 0;
    Status s = Size(&dev_size);
    if (!s.ok()) return s;
    if (dev_size < size) return Status::OutOfRange("block device smaller than requested size");
    return Status::Ok();
  }
  int ret = ::posix_fallocate(fd_, 0, static_cast<off_t>(size));
  if (ret == 0) return Status::Ok();
  // Some filesystems (e.g. tmpfs on old kernels) lack fallocate; fall back to ftruncate.
  if (ret == EOPNOTSUPP || ret == EINVAL) {
    if (::ftruncate(fd_, static_cast<off_t>(size)) == 0) return Status::Ok();
    return Status::FromErrno("ftruncate", errno);
  }
  return Status::FromErrno("posix_fallocate", ret);
}

Status File::Sync() {
  if (fd_ < 0) return Status::InvalidArgument("file not open");
  if (::fsync(fd_) != 0) return Status::FromErrno("fsync", errno);
  return Status::Ok();
}

Status File::Close() {
  if (fd_ < 0) return Status::Ok();
  int ret = ::close(fd_);
  fd_ = -1;
  if (ret != 0) return Status::FromErrno("close", errno);
  return Status::Ok();
}

}  // namespace ringstore
