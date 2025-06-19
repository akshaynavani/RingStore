#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <future>
#include <thread>
#include <vector>

#include "ringstore/aligned_buffer.h"
#include "ringstore/buffer_pool.h"
#include "ringstore/file.h"
#include "ringstore/io_engine.h"
#include "test_util.h"

namespace ringstore {
namespace {

using ringstore::testing::TempPath;

constexpr std::uint32_t kBlock = 4096;

class IoEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    REQUIRE_URING();
    FileOptions fo;
    fo.create = true;
    fo.truncate = true;
    ASSERT_TRUE(File::Open(tmp_.path(), fo, &file_).ok());
    ASSERT_TRUE(file_.Allocate(kBlock * 256).ok());
    EngineOptions eo;
    eo.ring.entries = 64;
    ASSERT_TRUE(engine_.Start(eo).ok());
  }

  void TearDown() override { engine_.Shutdown(); }

  static void Pattern(std::uint8_t* p, std::uint32_t len, std::uint32_t seed) {
    for (std::uint32_t i = 0; i < len; ++i) p[i] = static_cast<std::uint8_t>((i * 131u) ^ seed);
  }

  TempPath tmp_;
  File file_;
  IoEngine engine_;
};

TEST_F(IoEngineTest, WriteThenReadRoundTrip) {
  AlignedBuffer out(kBlock);
  AlignedBuffer in(kBlock);
  Pattern(out.data(), kBlock, 7);

  IoResult w = engine_.Write(file_.fd(), out.data(), kBlock, 3 * kBlock);
  ASSERT_TRUE(w.status.ok()) << w.status.ToString();
  EXPECT_EQ(w.bytes, kBlock);

  IoResult r = engine_.Read(file_.fd(), in.data(), kBlock, 3 * kBlock);
  ASSERT_TRUE(r.status.ok()) << r.status.ToString();
  EXPECT_EQ(r.bytes, kBlock);
  EXPECT_EQ(std::memcmp(out.data(), in.data(), kBlock), 0);
  EXPECT_GT(r.latency_ns, 0u);

  EXPECT_TRUE(engine_.Fsync(file_.fd()).status.ok());
  EXPECT_TRUE(engine_.Fsync(file_.fd(), /*datasync=*/true).status.ok());
}

TEST_F(IoEngineTest, CallbackFiresExactlyOnce) {
  AlignedBuffer buf(kBlock);
  std::atomic<int> calls{0};
  std::promise<void> done;
  ASSERT_TRUE(engine_
                  .Submit(IoRequest::Read(file_.fd(), buf.data(), kBlock, 0,
                                          [&](const IoResult& r) {
                                            EXPECT_TRUE(r.status.ok());
                                            if (++calls == 1) done.set_value();
                                          }))
                  .ok());
  done.get_future().wait();
  engine_.Shutdown();
  EXPECT_EQ(calls.load(), 1);
}

TEST_F(IoEngineTest, BatchSubmitWritesEveryBlock) {
  constexpr int kN = 128;  // more than max_inflight (64) to exercise back-pressure
  std::vector<AlignedBuffer> bufs;
  std::vector<IoRequest> reqs;
  std::atomic<int> ok{0};
  std::promise<void> all_done;
  std::atomic<int> remaining{kN};
  for (int i = 0; i < kN; ++i) {
    bufs.emplace_back(kBlock);
    Pattern(bufs.back().data(), kBlock, static_cast<std::uint32_t>(i));
    reqs.push_back(IoRequest::Write(file_.fd(), bufs.back().data(), kBlock,
                                    static_cast<std::uint64_t>(i) * kBlock,
                                    [&](const IoResult& r) {
                                      if (r.status.ok() && r.bytes == kBlock) ++ok;
                                      if (--remaining == 0) all_done.set_value();
                                    }));
  }
  ASSERT_TRUE(engine_.SubmitBatch(std::move(reqs)).ok());
  all_done.get_future().wait();
  EXPECT_EQ(ok.load(), kN);

  AlignedBuffer in(kBlock);
  for (int i = 0; i < kN; ++i) {
    ASSERT_TRUE(engine_.Read(file_.fd(), in.data(), kBlock, static_cast<std::uint64_t>(i) * kBlock).status.ok());
    ASSERT_EQ(std::memcmp(in.data(), bufs[static_cast<std::size_t>(i)].data(), kBlock), 0) << i;
  }
  EngineStats st = engine_.stats();
  EXPECT_LE(st.inflight_high_water, engine_.max_inflight());
  EXPECT_EQ(st.errors, 0u);
}

TEST_F(IoEngineTest, ConcurrentSubmittersStress) {
  constexpr int kThreads = 8;
  constexpr int kOpsPerThread = 200;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      AlignedBuffer out(kBlock);
      AlignedBuffer in(kBlock);
      // Each thread owns a disjoint set of blocks so content checks are exact.
      for (int i = 0; i < kOpsPerThread; ++i) {
        const std::uint64_t block = static_cast<std::uint64_t>(t) * 32 + (i % 32);
        Pattern(out.data(), kBlock, static_cast<std::uint32_t>(t * 1000 + i));
        auto w = engine_.WriteAsync(file_.fd(), out.data(), kBlock, block * kBlock).get();
        auto r = engine_.ReadAsync(file_.fd(), in.data(), kBlock, block * kBlock).get();
        if (!w.status.ok() || !r.status.ok() || std::memcmp(out.data(), in.data(), kBlock) != 0) {
          ++failures;
        }
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(failures.load(), 0);
  EngineStats st = engine_.stats();
  EXPECT_EQ(st.completed, st.submitted);
  EXPECT_EQ(st.submitted, static_cast<std::uint64_t>(kThreads * kOpsPerThread * 2));
}

TEST_F(IoEngineTest, ReadPastEofReturnsShortTransfer) {
  AlignedBuffer buf(kBlock * 2);
  // File is 256 blocks; read the last block plus one beyond.
  IoResult r = engine_.Read(file_.fd(), buf.data(), kBlock * 2, 255ull * kBlock);
  ASSERT_TRUE(r.status.ok());
  EXPECT_EQ(r.bytes, kBlock);
}

TEST_F(IoEngineTest, KernelErrorsPropagateAsIoError) {
  AlignedBuffer buf(kBlock);
  // A valid-looking but closed descriptor: the kernel rejects it with EBADF.
  IoResult r = engine_.Read(987654, buf.data(), kBlock, 0);
  EXPECT_EQ(r.status.code(), StatusCode::kIoError);
  EXPECT_EQ(r.status.sys_errno(), EBADF);
  EXPECT_EQ(engine_.stats().errors, 1u);
}

TEST_F(IoEngineTest, ValidationRejectsBadRequests) {
  AlignedBuffer buf(kBlock);
  EXPECT_EQ(engine_.Submit(IoRequest::Read(-1, buf.data(), kBlock, 0)).code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(engine_.Submit(IoRequest::Read(file_.fd(), nullptr, kBlock, 0)).code(),
            StatusCode::kInvalidArgument);
  IoRequest fixed = IoRequest::Read(file_.fd(), buf.data(), kBlock, 0);
  fixed.buf_index = 0;  // no buffers registered
  EXPECT_EQ(engine_.Submit(std::move(fixed)).code(), StatusCode::kInvalidArgument);
}

TEST_F(IoEngineTest, SubmitAfterShutdownIsRejected) {
  engine_.Shutdown();
  EXPECT_FALSE(engine_.running());
  AlignedBuffer buf(kBlock);
  EXPECT_EQ(engine_.Submit(IoRequest::Read(file_.fd(), buf.data(), kBlock, 0)).code(),
            StatusCode::kShutdown);
  IoResult r = engine_.Read(file_.fd(), buf.data(), kBlock, 0);
  EXPECT_EQ(r.status.code(), StatusCode::kShutdown);
}

TEST_F(IoEngineTest, ShutdownDrainsInflightRequests) {
  std::atomic<int> completed{0};
  std::vector<AlignedBuffer> bufs;
  for (int i = 0; i < 32; ++i) {
    bufs.emplace_back(kBlock);
    ASSERT_TRUE(engine_
                    .Submit(IoRequest::Write(file_.fd(), bufs.back().data(), kBlock,
                                             static_cast<std::uint64_t>(i) * kBlock,
                                             [&](const IoResult&) { ++completed; }))
                    .ok());
  }
  engine_.Shutdown();
  EXPECT_EQ(completed.load(), 32);
}

TEST_F(IoEngineTest, CallbacksCanChainFollowUpRequests) {
  AlignedBuffer buf(kBlock);
  std::atomic<int> hops{0};
  std::promise<void> done;
  std::function<void(const IoResult&)> step = [&](const IoResult& r) {
    ASSERT_TRUE(r.status.ok());
    if (++hops == 10) {
      done.set_value();
      return;
    }
    ASSERT_TRUE(engine_.Submit(IoRequest::Read(file_.fd(), buf.data(), kBlock,
                                               static_cast<std::uint64_t>(hops.load()) * kBlock, step))
                    .ok());
  };
  ASSERT_TRUE(engine_.Submit(IoRequest::Read(file_.fd(), buf.data(), kBlock, 0, step)).ok());
  done.get_future().wait();
  EXPECT_EQ(hops.load(), 10);
}

TEST_F(IoEngineTest, FixedBuffersAndFiles) {
  BufferPool pool(4, kBlock);
  Status s = engine_.RegisterBufferPool(pool);
  if (s.sys_errno() == ENOMEM || s.sys_errno() == EPERM) {
    GTEST_SKIP() << "RLIMIT_MEMLOCK too low: " << s.ToString();
  }
  ASSERT_TRUE(s.ok()) << s.ToString();
  ASSERT_TRUE(engine_.RegisterFiles({file_.fd()}).ok());

  PooledBuffer out = pool.Acquire();
  PooledBuffer in = pool.Acquire();
  Pattern(out.data(), kBlock, 99);

  IoRequest w = IoRequest::Write(0, out.data(), kBlock, 10 * kBlock);
  w.fixed_file = true;
  w.buf_index = out.index();
  ASSERT_TRUE(engine_.SubmitAsync(std::move(w)).get().status.ok());

  IoRequest r = IoRequest::Read(0, in.data(), kBlock, 10 * kBlock);
  r.fixed_file = true;
  r.buf_index = in.index();
  IoResult res = engine_.SubmitAsync(std::move(r)).get();
  ASSERT_TRUE(res.status.ok()) << res.status.ToString();
  EXPECT_EQ(std::memcmp(out.data(), in.data(), kBlock), 0);

  // Out-of-range fixed buffer index is rejected by the kernel.
  IoRequest bad = IoRequest::Read(0, in.data(), kBlock, 0);
  bad.fixed_file = true;
  bad.buf_index = 100;
  EXPECT_FALSE(engine_.SubmitAsync(std::move(bad)).get().status.ok());

  engine_.Shutdown();
}

TEST(IoEngineLifecycleTest, StartTwiceFails) {
  REQUIRE_URING();
  IoEngine e;
  ASSERT_TRUE(e.Start().ok());
  EXPECT_EQ(e.Start().code(), StatusCode::kInvalidArgument);
  e.Shutdown();
  e.Shutdown();  // idempotent
}

}  // namespace
}  // namespace ringstore
