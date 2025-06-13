#include "workload.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "ringstore/aligned_buffer.h"
#include "ringstore/buffer_pool.h"
#include "ringstore/file.h"
#include "ringstore/io_engine.h"

namespace ringstore {
namespace bench {

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t NowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

std::uint64_t XorShift(std::uint64_t& s) {
  s ^= s << 13;
  s ^= s >> 7;
  s ^= s << 17;
  return s;
}

void FillRandom(std::uint8_t* p, std::size_t n, std::uint64_t seed) {
  std::uint64_t s = seed | 1;
  for (std::size_t i = 0; i + 8 <= n; i += 8) {
    std::uint64_t v = XorShift(s);
    std::memcpy(p + i, &v, 8);
  }
}

unsigned NextPow2(unsigned v) {
  unsigned p = 1;
  while (p < v) p <<= 1;
  return p;
}

Status ValidateSpec(const WorkloadSpec& spec) {
  if (spec.path.empty()) return Status::InvalidArgument("no target path");
  if (spec.block_size == 0 || spec.block_size % 512 != 0) {
    return Status::InvalidArgument("block size must be a non-zero multiple of 512");
  }
  if (spec.file_size < spec.block_size) return Status::InvalidArgument("file smaller than block");
  if (spec.queue_depth == 0) return Status::InvalidArgument("queue depth must be > 0");
  if (spec.read_pct < 0 || spec.read_pct > 100) return Status::InvalidArgument("read_pct in [0,100]");
  return Status::Ok();
}

struct Timing {
  std::uint64_t start_ns;
  std::uint64_t measure_ns;   // warmup ends here
  std::uint64_t deadline_ns;  // stop issuing new I/O here
};

Timing MakeTiming(const WorkloadSpec& spec) {
  Timing t;
  t.start_ns = NowNs();
  t.measure_ns = t.start_ns + static_cast<std::uint64_t>(spec.warmup_s * 1e9);
  t.deadline_ns = t.measure_ns + static_cast<std::uint64_t>(spec.duration_s * 1e9);
  return t;
}

// Picks the next block for a request: shared cursor for sequential, per-slot
// RNG for random so slots never contend.
std::uint64_t NextBlock(const WorkloadSpec& spec, std::uint64_t nblocks,
                        std::atomic<std::uint64_t>& cursor, std::uint64_t& rng) {
  if (spec.pattern == Pattern::kSequential) {
    return cursor.fetch_add(1, std::memory_order_relaxed) % nblocks;
  }
  return XorShift(rng) % nblocks;
}

}  // namespace

const char* PatternName(Pattern p) { return p == Pattern::kSequential ? "seq" : "rand"; }
const char* EngineName(EngineKind e) { return e == EngineKind::kUring ? "io_uring" : "psync"; }

Status PrepareTarget(const WorkloadSpec& spec) {
  Status s = ValidateSpec(spec);
  if (!s.ok()) return s;
  FileOptions fo;
  fo.create = true;
  File f;
  s = File::Open(spec.path, fo, &f);
  if (!s.ok()) return s;
  if (f.is_block_device()) {
    std::uint64_t dev = 0;
    s = f.Size(&dev);
    if (!s.ok()) return s;
    if (dev < spec.file_size) return Status::OutOfRange("device smaller than requested size");
    return Status::Ok();
  }
  std::uint64_t cur = 0;
  s = f.Size(&cur);
  if (!s.ok()) return s;
  if (cur >= spec.file_size) return Status::Ok();

  // Write real data (not fallocate) so the filesystem has allocated, written
  // extents; reads of unwritten extents short-circuit and inflate numbers.
  constexpr std::size_t kChunk = 1 << 20;
  AlignedBuffer chunk(kChunk);
  FillRandom(chunk.data(), kChunk, spec.seed);
  for (std::uint64_t off = cur - (cur % kChunk); off < spec.file_size; off += kChunk) {
    std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, spec.file_size - off));
    std::size_t done = 0;
    while (done < n) {
      ssize_t w = ::pwrite(f.fd(), chunk.data() + done, n - done, static_cast<off_t>(off + done));
      if (w < 0) {
        if (errno == EINTR) continue;
        return Status::FromErrno("pwrite", errno);
      }
      done += static_cast<std::size_t>(w);
    }
  }
  return f.Sync();
}

Status RunUring(const WorkloadSpec& spec, WorkloadResult* out) {
  Status s = ValidateSpec(spec);
  if (!s.ok()) return s;

  FileOptions fo;
  fo.direct = spec.direct;
  fo.read_only = spec.read_pct == 100;
  File file;
  s = File::Open(spec.path, fo, &file);
  if (!s.ok()) return s;

  IoEngine engine;
  EngineOptions eo;
  eo.ring.entries = NextPow2(spec.queue_depth);
  eo.ring.sqpoll = spec.sqpoll;
  eo.max_inflight = spec.queue_depth;
  s = engine.Start(eo);
  if (!s.ok()) return s;

  BufferPool pool(spec.queue_depth, spec.block_size);
  if (spec.fixed_buffers) {
    s = engine.RegisterBufferPool(pool);
    if (!s.ok()) return s;
  }
  if (spec.fixed_files) {
    s = engine.RegisterFiles({file.fd()});
    if (!s.ok()) return s;
  }

  struct Slot {
    PooledBuffer buf;
    std::uint64_t rng = 0;
  };

  struct Shared {
    const WorkloadSpec* spec;
    IoEngine* engine;
    int fd;
    std::uint64_t nblocks;
    Timing timing;
    std::atomic<std::uint64_t> cursor{0};
    std::atomic<std::uint64_t> issued{0};
    // Touched only on the reaper thread while running.
    WorkloadResult result;
    std::uint64_t last_completion_ns = 0;
    std::mutex mu;
    std::condition_variable cv;
    unsigned active = 0;
  } sh;
  sh.spec = &spec;
  sh.engine = &engine;
  sh.fd = spec.fixed_files ? 0 : file.fd();
  sh.nblocks = spec.file_size / spec.block_size;
  sh.result.engine = EngineKind::kUring;

  std::vector<Slot> slots(spec.queue_depth);
  for (unsigned i = 0; i < spec.queue_depth; ++i) {
    slots[i].buf = pool.Acquire();
    slots[i].rng = (spec.seed + 1) * 0x9E3779B97F4A7C15ull + i;
    FillRandom(slots[i].buf.data(), spec.block_size, spec.seed + i);
  }

  auto finish_slot = [&sh] {
    std::lock_guard<std::mutex> lock(sh.mu);
    if (--sh.active == 0) sh.cv.notify_all();
  };

  // Self-referencing issue function: each completion issues the next request.
  std::function<void(Slot*)> issue = [&](Slot* slot) {
    if (spec.max_ops != 0 && sh.issued.fetch_add(1) >= spec.max_ops) {
      finish_slot();
      return;
    }
    const bool is_read = static_cast<int>(XorShift(slot->rng) % 100) < spec.read_pct;
    const std::uint64_t block = NextBlock(spec, sh.nblocks, sh.cursor, slot->rng);
    IoRequest req = is_read ? IoRequest::Read(sh.fd, slot->buf.data(), spec.block_size,
                                              block * spec.block_size)
                            : IoRequest::Write(sh.fd, slot->buf.data(), spec.block_size,
                                               block * spec.block_size);
    req.fixed_file = spec.fixed_files;
    if (spec.fixed_buffers) req.buf_index = slot->buf.index();
    req.callback = [&, slot, is_read](const IoResult& r) {
      const std::uint64_t now = NowNs();
      if (now >= sh.timing.measure_ns) {
        WorkloadResult& res = sh.result;
        if (r.status.ok()) {
          ++res.ops;
          res.bytes += r.bytes;
          ++(is_read ? res.reads : res.writes);
          res.latency.Record(r.latency_ns);
        } else {
          ++res.errors;
        }
        sh.last_completion_ns = now;
      }
      if (now < sh.timing.deadline_ns) {
        issue(slot);
      } else {
        finish_slot();
      }
    };
    if (!sh.engine->Submit(std::move(req)).ok()) finish_slot();
  };

  sh.active = spec.queue_depth;
  sh.timing = MakeTiming(spec);
  for (auto& slot : slots) issue(&slot);

  {
    std::unique_lock<std::mutex> lock(sh.mu);
    sh.cv.wait(lock, [&sh] { return sh.active == 0; });
  }
  const std::uint64_t end = std::max(sh.last_completion_ns, sh.timing.measure_ns);
  *out = sh.result;
  out->seconds = static_cast<double>(end - sh.timing.measure_ns) / 1e9;
  out->submit_syscalls = engine.stats().submit_syscalls;

  for (auto& slot : slots) slot.buf.Reset();
  engine.Shutdown();
  return Status::Ok();
}

Status RunSync(const WorkloadSpec& spec, WorkloadResult* out) {
  Status s = ValidateSpec(spec);
  if (!s.ok()) return s;

  FileOptions fo;
  fo.direct = spec.direct;
  fo.read_only = spec.read_pct == 100;
  File file;
  s = File::Open(spec.path, fo, &file);
  if (!s.ok()) return s;

  const std::uint64_t nblocks = spec.file_size / spec.block_size;
  const Timing timing = MakeTiming(spec);
  std::atomic<std::uint64_t> cursor{0};
  std::atomic<std::uint64_t> issued{0};
  std::vector<WorkloadResult> per_thread(spec.queue_depth);
  std::vector<std::uint64_t> last_ns(spec.queue_depth, 0);
  std::vector<std::thread> threads;
  threads.reserve(spec.queue_depth);

  for (unsigned t = 0; t < spec.queue_depth; ++t) {
    threads.emplace_back([&, t] {
      AlignedBuffer buf(spec.block_size);
      FillRandom(buf.data(), spec.block_size, spec.seed + t);
      std::uint64_t rng = (spec.seed + 1) * 0x9E3779B97F4A7C15ull + t;
      WorkloadResult& res = per_thread[t];
      for (;;) {
        std::uint64_t now = NowNs();
        if (now >= timing.deadline_ns) break;
        if (spec.max_ops != 0 && issued.fetch_add(1) >= spec.max_ops) break;
        const bool is_read = static_cast<int>(XorShift(rng) % 100) < spec.read_pct;
        const std::uint64_t block = NextBlock(spec, nblocks, cursor, rng);
        const auto off = static_cast<off_t>(block * spec.block_size);

        const std::uint64_t t0 = NowNs();
        ssize_t n = is_read ? ::pread(file.fd(), buf.data(), spec.block_size, off)
                            : ::pwrite(file.fd(), buf.data(), spec.block_size, off);
        const std::uint64_t t1 = NowNs();
        if (t1 < timing.measure_ns) continue;
        if (n == static_cast<ssize_t>(spec.block_size)) {
          ++res.ops;
          res.bytes += spec.block_size;
          ++(is_read ? res.reads : res.writes);
          res.latency.Record(t1 - t0);
        } else {
          ++res.errors;
        }
        last_ns[t] = t1;
      }
    });
  }
  for (auto& th : threads) th.join();

  WorkloadResult total;
  total.engine = EngineKind::kSync;
  std::uint64_t end = timing.measure_ns;
  for (unsigned t = 0; t < spec.queue_depth; ++t) {
    const auto& r = per_thread[t];
    total.ops += r.ops;
    total.reads += r.reads;
    total.writes += r.writes;
    total.bytes += r.bytes;
    total.errors += r.errors;
    total.latency.Merge(r.latency);
    end = std::max(end, last_ns[t]);
  }
  total.seconds = static_cast<double>(end - timing.measure_ns) / 1e9;
  // Every pread/pwrite is one syscall.
  total.submit_syscalls = total.ops + total.errors;
  *out = std::move(total);
  return Status::Ok();
}

}  // namespace bench
}  // namespace ringstore
