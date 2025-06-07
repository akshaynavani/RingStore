#include "ringstore/status.h"

#include <cstring>

namespace ringstore {

const char* StatusCodeName(StatusCode code) {
  switch (code) {
    case StatusCode::kOk: return "OK";
    case StatusCode::kIoError: return "IoError";
    case StatusCode::kInvalidArgument: return "InvalidArgument";
    case StatusCode::kOutOfRange: return "OutOfRange";
    case StatusCode::kCorruption: return "Corruption";
    case StatusCode::kNotFound: return "NotFound";
    case StatusCode::kShutdown: return "Shutdown";
    case StatusCode::kResourceExhausted: return "ResourceExhausted";
  }
  return "Unknown";
}

Status Status::FromErrno(const std::string& context, int err) {
  if (err < 0) err = -err;
  return Status(StatusCode::kIoError, context + ": " + std::strerror(err), err);
}

std::string Status::ToString() const {
  if (ok()) return "OK";
  std::string out = StatusCodeName(code_);
  if (!message_.empty()) {
    out += ": ";
    out += message_;
  }
  return out;
}

}  // namespace ringstore
