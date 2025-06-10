// Minimal walkthrough of the IoEngine API: write a few blocks with callbacks,
// read them back through registered (fixed) buffers, then fsync.
//
//   ./ringstore_example /tmp/ringstore_example.dat

#include <atomic>
#include <cstdio>
#include <cstring>
#include <future>
#include <vector>

#include "ringstore/buffer_pool.h"
#include "ringstore/file.h"
#include "ringstore/io_engine.h"

using namespace ringstore;

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "/tmp/ringstore_example.dat";
  constexpr std::uint32_t kBlock = 4096;
  constexpr int kBlocks = 8;

  File file;
  FileOptions fopts;
  fopts.create = true;
  fopts.truncate = true;
  Status s = File::Open(path, fopts, &file);
  if (!s.ok()) {
    std::fprintf(stderr, "open failed: %s\n", s.ToString().c_str());
    return 1;
  }
  file.Allocate(kBlock * kBlocks);

  IoEngine engine;
  EngineOptions eopts;
  eopts.ring.entries = 64;
  if (!(s = engine.Start(eopts)).ok()) {
    std::fprintf(stderr, "engine start failed: %s\n", s.ToString().c_str());
    return 1;
  }

  BufferPool pool(kBlocks, kBlock);
  if (!(s = engine.RegisterBufferPool(pool)).ok()) {
    std::fprintf(stderr, "buffer registration failed: %s\n", s.ToString().c_str());
    return 1;
  }

  // Write: fill each pooled buffer with its block number. Each write reports
  // through a callback and a promise; io_uring does not order independent
  // requests, so we wait for every write before issuing the fsync.
  std::vector<PooledBuffer> bufs;
  std::vector<std::future<IoResult>> pending;
  std::atomic<int> write_errors{0};
  for (int i = 0; i < kBlocks; ++i) {
    bufs.push_back(pool.Acquire());
    std::memset(bufs.back().data(), 'A' + i, kBlock);
    IoRequest r = IoRequest::Write(file.fd(), bufs.back().data(), kBlock,
                                   static_cast<std::uint64_t>(i) * kBlock,
                                   [&write_errors](const IoResult& res) {
                                     if (!res.status.ok()) ++write_errors;
                                   });
    r.buf_index = bufs.back().index();
    pending.push_back(engine.SubmitAsync(std::move(r)));
  }
  for (auto& f : pending) f.get();
  engine.Fsync(file.fd());

  // Read back through the same fixed buffers after clearing them.
  int mismatches = 0;
  for (int i = 0; i < kBlocks; ++i) {
    std::memset(bufs[i].data(), 0, kBlock);
    IoResult r = engine.ReadAsync(file.fd(), bufs[i].data(), kBlock,
                                  static_cast<std::uint64_t>(i) * kBlock, bufs[i].index())
                     .get();
    if (!r.status.ok() || r.bytes != kBlock || bufs[i].data()[0] != 'A' + i) ++mismatches;
  }

  EngineStats st = engine.stats();
  std::printf("write errors=%d read mismatches=%d submitted=%llu completed=%llu syscalls=%llu\n",
              write_errors.load(), mismatches, static_cast<unsigned long long>(st.submitted),
              static_cast<unsigned long long>(st.completed),
              static_cast<unsigned long long>(st.submit_syscalls));

  bufs.clear();
  engine.Shutdown();
  return (write_errors == 0 && mismatches == 0) ? 0 : 1;
}
