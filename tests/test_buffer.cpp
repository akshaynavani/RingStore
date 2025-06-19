#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include "ringstore/aligned_buffer.h"
#include "ringstore/buffer_pool.h"

namespace ringstore {
namespace {

TEST(AlignedBufferTest, AllocatesAlignedZeroedMemory) {
  for (std::size_t align : {512u, 4096u, 65536u}) {
    AlignedBuffer buf(10000, align);
    ASSERT_FALSE(buf.empty());
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(buf.data()) % align, 0u);
    EXPECT_EQ(buf.size(), 10000u);
    for (std::size_t i = 0; i < buf.size(); ++i) ASSERT_EQ(buf.data()[i], 0);
  }
}

TEST(AlignedBufferTest, RejectsNonPowerOfTwoAlignment) {
  EXPECT_THROW(AlignedBuffer(4096, 3000), std::invalid_argument);
}

TEST(AlignedBufferTest, MoveTransfersOwnership) {
  AlignedBuffer a(4096);
  a.Fill(0xAB);
  std::uint8_t* p = a.data();
  AlignedBuffer b(std::move(a));
  EXPECT_TRUE(a.empty());
  EXPECT_EQ(b.data(), p);
  EXPECT_EQ(b.data()[100], 0xAB);

  AlignedBuffer c;
  c = std::move(b);
  EXPECT_TRUE(b.empty());
  EXPECT_EQ(c.data(), p);
}

TEST(BufferPoolTest, HandsOutDistinctAlignedBuffers) {
  BufferPool pool(8, 4096);
  EXPECT_EQ(pool.capacity(), 8u);
  std::vector<PooledBuffer> held;
  std::set<int> indices;
  for (int i = 0; i < 8; ++i) {
    held.push_back(pool.Acquire());
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(held.back().data()) % 4096, 0u);
    indices.insert(held.back().index());
  }
  EXPECT_EQ(indices.size(), 8u);
  EXPECT_EQ(pool.available(), 0u);
  EXPECT_FALSE(pool.TryAcquire(std::chrono::milliseconds(5)).has_value());
}

TEST(BufferPoolTest, ReleaseOnDestructionReturnsBuffer) {
  BufferPool pool(1, 4096);
  {
    PooledBuffer b = pool.Acquire();
    EXPECT_EQ(pool.available(), 0u);
  }
  EXPECT_EQ(pool.available(), 1u);
  auto again = pool.TryAcquire();
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(again->index(), 0);
}

TEST(BufferPoolTest, IovecsMatchHandles) {
  BufferPool pool(4, 8192);
  const auto& iov = pool.iovecs();
  ASSERT_EQ(iov.size(), 4u);
  PooledBuffer b = pool.Acquire();
  EXPECT_EQ(iov[static_cast<std::size_t>(b.index())].iov_base, b.data());
  EXPECT_EQ(iov[static_cast<std::size_t>(b.index())].iov_len, 8192u);
}

TEST(BufferPoolTest, BlockedAcquireWakesOnRelease) {
  BufferPool pool(1, 4096);
  PooledBuffer held = pool.Acquire();
  std::thread waiter([&pool] {
    PooledBuffer b = pool.Acquire();
    EXPECT_TRUE(b.valid());
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  held.Reset();
  waiter.join();
  EXPECT_EQ(pool.available(), 1u);
}

TEST(BufferPoolTest, RejectsUnalignedBufferSize) {
  EXPECT_THROW(BufferPool(4, 1000), std::invalid_argument);
  EXPECT_THROW(BufferPool(0, 4096), std::invalid_argument);
}

}  // namespace
}  // namespace ringstore
