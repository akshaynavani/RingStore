#pragma once

#include <cstdint>
#include <string>

#include "histogram.h"
#include "ringstore/status.h"

namespace ringstore {
namespace bench {

enum class Pattern { kSequential, kRandom };
enum class EngineKind { kUring, kSync };

struct WorkloadSpec {
  std::string path;
  std::uint64_t file_size = 1ull << 30;  // bytes of the target region
  std::uint32_t block_size = 4096;
  unsigned queue_depth = 32;
  Pattern pattern = Pattern::kRandom;
  int read_pct = 100;            // 100 = pure read, 0 = pure write
  double duration_s = 10.0;
  std::uint64_t max_ops = 0;     // 0 = run for duration only
  double warmup_s = 1.0;         // excluded from results
  bool direct = true;
  bool fixed_buffers = false;
  bool fixed_files = false;
  bool sqpoll = false;
  std::uint64_t seed = 42;
};

struct WorkloadResult {
  EngineKind engine = EngineKind::kUring;
  std::uint64_t ops = 0;
  std::uint64_t reads = 0;
  std::uint64_t writes = 0;
  std::uint64_t bytes = 0;
  std::uint64_t errors = 0;
  double seconds = 0.0;
  std::uint64_t submit_syscalls = 0;
  LatencyHistogram latency;  // nanoseconds

  double iops() const { return seconds > 0 ? static_cast<double>(ops) / seconds : 0.0; }
  double mib_per_s() const {
    return seconds > 0 ? static_cast<double>(bytes) / (1024.0 * 1024.0) / seconds : 0.0;
  }
};

// Creates (if needed) and fills the target file so reads touch real,
// allocated blocks instead of holes. No-op for block devices.
Status PrepareTarget(const WorkloadSpec& spec);

// Closed-loop io_uring run: exactly `queue_depth` requests are kept in flight;
// each completion immediately issues the next request for that slot.
Status RunUring(const WorkloadSpec& spec, WorkloadResult* out);

// Baseline: `queue_depth` threads each issuing blocking pread/pwrite.
Status RunSync(const WorkloadSpec& spec, WorkloadResult* out);

const char* PatternName(Pattern p);
const char* EngineName(EngineKind e);

}  // namespace bench
}  // namespace ringstore
