#pragma once

#include <string>
#include <utility>

namespace ringstore {

enum class StatusCode {
  kOk = 0,
  kIoError,
  kInvalidArgument,
  kOutOfRange,
  kCorruption,
  kNotFound,
  kShutdown,
  kResourceExhausted,
};

// Lightweight status type. Carries an optional errno for I/O errors so callers
// can distinguish e.g. EAGAIN from EIO without parsing messages.
class Status {
 public:
  Status() = default;
  Status(StatusCode code, std::string message, int err = 0)
      : code_(code), message_(std::move(message)), errno_(err) {}

  static Status Ok() { return Status(); }
  static Status IoError(std::string msg, int err = 0) {
    return Status(StatusCode::kIoError, std::move(msg), err);
  }
  static Status InvalidArgument(std::string msg) {
    return Status(StatusCode::kInvalidArgument, std::move(msg));
  }
  static Status OutOfRange(std::string msg) {
    return Status(StatusCode::kOutOfRange, std::move(msg));
  }
  static Status Corruption(std::string msg) {
    return Status(StatusCode::kCorruption, std::move(msg));
  }
  static Status NotFound(std::string msg) {
    return Status(StatusCode::kNotFound, std::move(msg));
  }
  static Status Shutdown(std::string msg) {
    return Status(StatusCode::kShutdown, std::move(msg));
  }
  static Status ResourceExhausted(std::string msg) {
    return Status(StatusCode::kResourceExhausted, std::move(msg));
  }

  // Builds an IoError from a (positive) errno value, appending strerror text.
  static Status FromErrno(const std::string& context, int err);

  bool ok() const { return code_ == StatusCode::kOk; }
  StatusCode code() const { return code_; }
  const std::string& message() const { return message_; }
  int sys_errno() const { return errno_; }

  std::string ToString() const;

 private:
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
  int errno_ = 0;
};

const char* StatusCodeName(StatusCode code);

}  // namespace ringstore
