#include <gtest/gtest.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <future>
#include <memory>
#include <vector>

#include "ringstore/aligned_buffer.h"
#include "ringstore/block_store.h"
#include "ringstore/crc32c.h"
#include "ringstore/io_engine.h"
#include "ringstore/layout.h"
#include "test_util.h"

namespace ringstore {
namespace {

using ringstore::testing::TempPath;

constexpr std::uint32_t kBs = 4096;
constexpr std::uint64_t kBlocks = 2048;  // > 1024 so the checksum region spans 2 blocks

void Pattern(std::uint8_t* p, std::size_t n, std::uint64_t seed) {
  for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<std::uint8_t>((i * 2654435761u) >> 3 ^ seed);
}

TEST(LayoutTest, ComputesRegions) {
  Layout l;
  ASSERT_TRUE(Layout::Compute(4096, 2048, &l).ok());
  EXPECT_EQ(l.csum_offset, 4096u);
  EXPECT_EQ(l.csum_blocks, 2u);  // 2048 * 4 bytes = 8 KiB
  EXPECT_EQ(l.data_offset, 3u * 4096);
  EXPECT_EQ(l.total_bytes(), 3u * 4096 + 2048u * 4096);
  EXPECT_EQ(l.data_block_offset(5), 3u * 4096 + 5u * 4096);
}

TEST(LayoutTest, RejectsBadGeometry) {
  Layout l;
  EXPECT_FALSE(Layout::Compute(4095, 10, &l).ok());
  EXPECT_FALSE(Layout::Compute(256, 10, &l).ok());
  EXPECT_FALSE(Layout::Compute(4096, 0, &l).ok());
}

TEST(LayoutTest, SuperblockRoundTripAndCorruption) {
  Layout l;
  ASSERT_TRUE(Layout::Compute(4096, 100, &l).ok());
  AlignedBuffer block(4096);
  Superblock sb = MakeSuperblock(l, 12345);
  std::memcpy(block.data(), &sb, sizeof(sb));

  Layout parsed;
  ASSERT_TRUE(ParseSuperblock(block.data(), 0, &parsed).ok());
  EXPECT_EQ(parsed.data_blocks, 100u);
  EXPECT_EQ(parsed.data_offset, l.data_offset);
  EXPECT_EQ(ParseSuperblock(block.data(), 512, &parsed).code(), StatusCode::kInvalidArgument);

  block.data()[20] ^= 0xFF;  // flip a byte inside data_blocks
  EXPECT_EQ(ParseSuperblock(block.data(), 0, &parsed).code(), StatusCode::kCorruption);

  block.Fill(0);
  EXPECT_EQ(ParseSuperblock(block.data(), 0, &parsed).code(), StatusCode::kCorruption);
}

class BlockStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    REQUIRE_URING();
    EngineOptions eo;
    eo.ring.entries = 64;
    ASSERT_TRUE(engine_.Start(eo).ok());
  }
  void TearDown() override {
    store_.reset();
    engine_.Shutdown();
  }

  BlockStoreOptions Opts() const {
    BlockStoreOptions o;
    o.block_size = kBs;
    o.num_blocks = kBlocks;
    o.direct_io = false;  // tmpfs and some CI filesystems reject O_DIRECT
    return o;
  }

  void CreateStore() {
    Status s = BlockStore::Create(tmp_.path(), Opts(), &engine_, &store_);
    ASSERT_TRUE(s.ok()) << s.ToString();
  }

  void Reopen() {
    ASSERT_TRUE(store_->Close().ok());
    store_.reset();
    Status s = BlockStore::Open(tmp_.path(), Opts(), &engine_, &store_);
    ASSERT_TRUE(s.ok()) << s.ToString();
  }

  TempPath tmp_;
  IoEngine engine_;
  std::unique_ptr<BlockStore> store_;
};

TEST_F(BlockStoreTest, CreateReportsGeometry) {
  CreateStore();
  EXPECT_EQ(store_->block_size(), kBs);
  EXPECT_EQ(store_->num_blocks(), kBlocks);
}

TEST_F(BlockStoreTest, SingleBlockRoundTrip) {
  CreateStore();
  AlignedBuffer out(kBs);
  AlignedBuffer in(kBs);
  Pattern(out.data(), kBs, 1);
  ASSERT_TRUE(store_->WriteBlock(42, out.data()).ok());
  ASSERT_TRUE(store_->ReadBlock(42, in.data()).ok());
  EXPECT_EQ(std::memcmp(out.data(), in.data(), kBs), 0);
}

TEST_F(BlockStoreTest, MultiBlockRoundTrip) {
  CreateStore();
  constexpr std::uint32_t kN = 16;
  AlignedBuffer out(kN * kBs);
  AlignedBuffer in(kN * kBs);
  Pattern(out.data(), out.size(), 2);
  ASSERT_TRUE(store_->WriteBlocks(1020, kN, out.data()).ok());  // spans csum-block boundary
  ASSERT_TRUE(store_->ReadBlocks(1020, kN, in.data()).ok());
  EXPECT_EQ(std::memcmp(out.data(), in.data(), out.size()), 0);
}

TEST_F(BlockStoreTest, UnwrittenBlocksReadWithoutChecksumError) {
  CreateStore();
  AlignedBuffer in(kBs);
  EXPECT_TRUE(store_->ReadBlock(7, in.data()).ok());
}

TEST_F(BlockStoreTest, RejectsOutOfRangeAndBadArgs) {
  CreateStore();
  AlignedBuffer buf(2 * kBs);
  EXPECT_EQ(store_->ReadBlock(kBlocks, buf.data()).code(), StatusCode::kOutOfRange);
  EXPECT_EQ(store_->WriteBlocks(kBlocks - 1, 2, buf.data()).code(), StatusCode::kOutOfRange);
  EXPECT_EQ(store_->ReadBlocks(0, 0, buf.data()).code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(store_->ReadBlock(0, nullptr).code(), StatusCode::kInvalidArgument);
}

TEST_F(BlockStoreTest, DataAndChecksumsPersistAcrossReopen) {
  CreateStore();
  AlignedBuffer out(kBs);
  for (std::uint64_t b : {0ull, 1ull, 1023ull, 1024ull, kBlocks - 1}) {
    Pattern(out.data(), kBs, b);
    ASSERT_TRUE(store_->WriteBlock(b, out.data()).ok());
  }
  Reopen();
  AlignedBuffer in(kBs);
  for (std::uint64_t b : {0ull, 1ull, 1023ull, 1024ull, kBlocks - 1}) {
    Pattern(out.data(), kBs, b);
    ASSERT_TRUE(store_->ReadBlock(b, in.data()).ok()) << b;
    EXPECT_EQ(std::memcmp(out.data(), in.data(), kBs), 0) << b;
  }
}

TEST_F(BlockStoreTest, DetectsOnDiskCorruption) {
  CreateStore();
  AlignedBuffer out(kBs);
  Pattern(out.data(), kBs, 77);
  ASSERT_TRUE(store_->WriteBlock(100, out.data()).ok());
  const std::uint64_t off = store_->layout().data_block_offset(100);
  Reopen();

  // Flip one byte behind the store's back.
  int fd = ::open(tmp_.path().c_str(), O_RDWR);
  ASSERT_GE(fd, 0);
  std::uint8_t byte = 0;
  ASSERT_EQ(::pread(fd, &byte, 1, static_cast<off_t>(off + 1234)), 1);
  byte ^= 0x10;
  ASSERT_EQ(::pwrite(fd, &byte, 1, static_cast<off_t>(off + 1234)), 1);
  ::fsync(fd);
  ::close(fd);

  AlignedBuffer in(kBs);
  EXPECT_EQ(store_->ReadBlock(100, in.data()).code(), StatusCode::kCorruption);
  EXPECT_EQ(store_->checksum_failures(), 1u);
  // Neighbouring blocks are unaffected.
  EXPECT_TRUE(store_->ReadBlock(101, in.data()).ok());
}

TEST_F(BlockStoreTest, VerificationCanBeDisabled) {
  CreateStore();
  AlignedBuffer out(kBs);
  Pattern(out.data(), kBs, 5);
  ASSERT_TRUE(store_->WriteBlock(3, out.data()).ok());
  const std::uint64_t off = store_->layout().data_block_offset(3);
  ASSERT_TRUE(store_->Close().ok());
  store_.reset();

  int fd = ::open(tmp_.path().c_str(), O_RDWR);
  ASSERT_GE(fd, 0);
  std::uint8_t junk = 0xEE;
  ASSERT_EQ(::pwrite(fd, &junk, 1, static_cast<off_t>(off)), 1);
  ::close(fd);

  BlockStoreOptions o = Opts();
  o.verify_checksums = false;
  ASSERT_TRUE(BlockStore::Open(tmp_.path(), o, &engine_, &store_).ok());
  AlignedBuffer in(kBs);
  EXPECT_TRUE(store_->ReadBlock(3, in.data()).ok());
  EXPECT_EQ(in.data()[0], 0xEE);
}

TEST_F(BlockStoreTest, OpenRejectsCorruptSuperblock) {
  CreateStore();
  ASSERT_TRUE(store_->Close().ok());
  store_.reset();

  int fd = ::open(tmp_.path().c_str(), O_RDWR);
  ASSERT_GE(fd, 0);
  std::uint8_t junk[8] = {0};
  ASSERT_EQ(::pwrite(fd, junk, sizeof(junk), 0), 8);  // wipe the magic
  ::close(fd);

  EXPECT_EQ(BlockStore::Open(tmp_.path(), Opts(), &engine_, &store_).code(),
            StatusCode::kCorruption);
}

TEST_F(BlockStoreTest, OpenRejectsTruncatedFile) {
  CreateStore();
  const std::uint64_t total = store_->layout().total_bytes();
  ASSERT_TRUE(store_->Close().ok());
  store_.reset();
  ASSERT_EQ(::truncate(tmp_.path().c_str(), static_cast<off_t>(total - kBs)), 0);
  EXPECT_EQ(BlockStore::Open(tmp_.path(), Opts(), &engine_, &store_).code(),
            StatusCode::kCorruption);
}

TEST_F(BlockStoreTest, ManyConcurrentAsyncWrites) {
  CreateStore();
  constexpr int kN = 512;
  std::vector<AlignedBuffer> bufs;
  bufs.reserve(kN);
  std::atomic<int> remaining{kN};
  std::atomic<int> failures{0};
  std::promise<void> done;
  for (int i = 0; i < kN; ++i) {
    bufs.emplace_back(kBs);
    Pattern(bufs.back().data(), kBs, static_cast<std::uint64_t>(i) + 1000);
    ASSERT_TRUE(store_
                    ->WriteBlockAsync(static_cast<std::uint64_t>(i) * 3, bufs.back().data(),
                                      [&](const Status& s) {
                                        if (!s.ok()) ++failures;
                                        if (--remaining == 0) done.set_value();
                                      })
                    .ok());
  }
  done.get_future().wait();
  EXPECT_EQ(failures.load(), 0);

  Reopen();
  AlignedBuffer in(kBs);
  for (int i = 0; i < kN; ++i) {
    ASSERT_TRUE(store_->ReadBlock(static_cast<std::uint64_t>(i) * 3, in.data()).ok()) << i;
    ASSERT_EQ(std::memcmp(in.data(), bufs[static_cast<std::size_t>(i)].data(), kBs), 0) << i;
  }
}

TEST_F(BlockStoreTest, DirectIoWhenSupported) {
  BlockStoreOptions o = Opts();
  o.direct_io = true;
  Status s = BlockStore::Create(tmp_.path(), o, &engine_, &store_);
  if (s.sys_errno() == EINVAL) GTEST_SKIP() << "filesystem does not support O_DIRECT";
  ASSERT_TRUE(s.ok()) << s.ToString();

  AlignedBuffer out(kBs);
  AlignedBuffer in(kBs);
  Pattern(out.data(), kBs, 9);
  ASSERT_TRUE(store_->WriteBlock(9, out.data()).ok());
  ASSERT_TRUE(store_->ReadBlock(9, in.data()).ok());
  EXPECT_EQ(std::memcmp(out.data(), in.data(), kBs), 0);

  // Unaligned user buffers are rejected up front under O_DIRECT.
  std::vector<std::uint8_t> unaligned(kBs + 1);
  std::uint8_t* p = unaligned.data();
  if (reinterpret_cast<std::uintptr_t>(p) % 512 == 0) ++p;
  EXPECT_EQ(store_->ReadBlock(9, p).code(), StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace ringstore
