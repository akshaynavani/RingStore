#include <gtest/gtest.h>

#include "ringstore/buffer_pool.h"
#include "ringstore/ring.h"
#include "test_util.h"

namespace ringstore {
namespace {

TEST(RingTest, InitAndDoubleInit) {
  REQUIRE_URING();
  Ring ring;
  RingOptions opts;
  opts.entries = 8;
  ASSERT_TRUE(ring.Init(opts).ok());
  EXPECT_TRUE(ring.initialized());
  EXPECT_EQ(ring.Init(opts).code(), StatusCode::kInvalidArgument);
}

TEST(RingTest, RejectsZeroEntries) {
  Ring ring;
  RingOptions opts;
  opts.entries = 0;
  EXPECT_EQ(ring.Init(opts).code(), StatusCode::kInvalidArgument);
}

TEST(RingTest, NopRoundTripCarriesUserData) {
  REQUIRE_URING();
  Ring ring;
  RingOptions opts;
  opts.entries = 8;
  ASSERT_TRUE(ring.Init(opts).ok());

  int tags[3] = {1, 2, 3};
  for (int& t : tags) {
    io_uring_sqe* sqe = ring.GetSqe();
    ASSERT_NE(sqe, nullptr);
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data(sqe, &t);
  }
  ASSERT_EQ(ring.Submit(), 3);

  int sum = 0;
  for (int i = 0; i < 3; ++i) {
    io_uring_cqe* cqe = nullptr;
    ASSERT_EQ(ring.WaitCqe(&cqe), 0);
    EXPECT_EQ(cqe->res, 0);
    sum += *static_cast<int*>(io_uring_cqe_get_data(cqe));
    ring.CqeSeen(cqe);
  }
  EXPECT_EQ(sum, 6);

  io_uring_cqe* none = nullptr;
  EXPECT_EQ(ring.PeekCqe(&none), -EAGAIN);
}

TEST(RingTest, SqFillsUpAtDepth) {
  REQUIRE_URING();
  Ring ring;
  RingOptions opts;
  opts.entries = 4;
  ASSERT_TRUE(ring.Init(opts).ok());
  for (int i = 0; i < 4; ++i) ASSERT_NE(ring.GetSqe(), nullptr);
  EXPECT_EQ(ring.GetSqe(), nullptr);
  EXPECT_EQ(ring.SqSpaceLeft(), 0u);
}

TEST(RingTest, ProbeReportsBasicOps) {
  REQUIRE_URING();
  Ring ring;
  RingOptions opts;
  opts.entries = 4;
  ASSERT_TRUE(ring.Init(opts).ok());
  EXPECT_TRUE(ring.SupportsOp(IORING_OP_NOP));
  EXPECT_TRUE(ring.SupportsOp(IORING_OP_READ));
  EXPECT_TRUE(ring.SupportsOp(IORING_OP_WRITE));
}

TEST(RingTest, RegisterAndUnregisterBuffers) {
  REQUIRE_URING();
  Ring ring;
  RingOptions opts;
  opts.entries = 8;
  ASSERT_TRUE(ring.Init(opts).ok());
  BufferPool pool(4, 4096);
  Status s = ring.RegisterBuffers(pool.iovecs());
  if (s.sys_errno() == ENOMEM || s.sys_errno() == EPERM) {
    GTEST_SKIP() << "RLIMIT_MEMLOCK too low to register buffers: " << s.ToString();
  }
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_TRUE(ring.buffers_registered());
  EXPECT_FALSE(ring.RegisterBuffers(pool.iovecs()).ok());
  EXPECT_TRUE(ring.UnregisterBuffers().ok());
  EXPECT_FALSE(ring.buffers_registered());
}

}  // namespace
}  // namespace ringstore
