#pragma once

#include <cstdint>

#include "ringstore/ring.h"

namespace ringstore {

struct EngineOptions {
  RingOptions ring;
  // Upper bound on requests in flight. 0 => ring.entries. Kept <= SQ depth so
  // the completion queue (2x SQ by default) can never overflow.
  unsigned max_inflight = 0;
  // How many times a request may be resubmitted on short I/O or EAGAIN before
  // it is failed with an IoError.
  unsigned max_resubmits = 16;
};

struct EngineStats {
  std::uint64_t submitted = 0;
  std::uint64_t completed = 0;
  std::uint64_t errors = 0;
  std::uint64_t resubmits = 0;
  std::uint64_t bytes_read = 0;
  std::uint64_t bytes_written = 0;
  std::uint64_t submit_syscalls = 0;
  std::uint64_t inflight = 0;
  std::uint64_t inflight_high_water = 0;
};

}  // namespace ringstore
