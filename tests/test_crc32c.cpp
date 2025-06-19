#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "ringstore/crc32c.h"

namespace ringstore {
namespace {

// Vectors from RFC 3720 (iSCSI) appendix B.4 and the standard check value.
TEST(Crc32cTest, StandardCheckValue) {
  const std::string s = "123456789";
  EXPECT_EQ(crc32c::Value(s.data(), s.size()), 0xE3069283u);
}

TEST(Crc32cTest, Rfc3720Vectors) {
  std::vector<std::uint8_t> buf(32);

  std::memset(buf.data(), 0x00, buf.size());
  EXPECT_EQ(crc32c::Value(buf.data(), buf.size()), 0x8A9136AAu);

  std::memset(buf.data(), 0xFF, buf.size());
  EXPECT_EQ(crc32c::Value(buf.data(), buf.size()), 0x62A8AB43u);

  for (std::size_t i = 0; i < 32; ++i) buf[i] = static_cast<std::uint8_t>(i);
  EXPECT_EQ(crc32c::Value(buf.data(), buf.size()), 0x46DD794Eu);

  for (std::size_t i = 0; i < 32; ++i) buf[i] = static_cast<std::uint8_t>(31 - i);
  EXPECT_EQ(crc32c::Value(buf.data(), buf.size()), 0x113FDB5Cu);
}

TEST(Crc32cTest, EmptyInputIsZero) { EXPECT_EQ(crc32c::Value(nullptr, 0), 0u); }

TEST(Crc32cTest, ExtendMatchesOneShot) {
  std::vector<std::uint8_t> data(1000);
  for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i * 31 + 7);
  const std::uint32_t whole = crc32c::Value(data.data(), data.size());
  for (std::size_t split : {1u, 7u, 8u, 9u, 500u, 999u}) {
    std::uint32_t c = crc32c::Value(data.data(), split);
    c = crc32c::Extend(c, data.data() + split, data.size() - split);
    EXPECT_EQ(c, whole) << "split=" << split;
  }
}

TEST(Crc32cTest, UnalignedInputGivesSameResult) {
  std::vector<std::uint8_t> backing(4096 + 16);
  for (std::size_t i = 0; i < backing.size(); ++i) backing[i] = static_cast<std::uint8_t>(i ^ 0x5A);
  std::vector<std::uint8_t> copy(backing.begin() + 3, backing.begin() + 3 + 4096);
  EXPECT_EQ(crc32c::Value(backing.data() + 3, 4096), crc32c::Value(copy.data(), copy.size()));
}

TEST(Crc32cTest, SingleBitFlipChangesChecksum) {
  std::vector<std::uint8_t> data(4096, 0x42);
  const std::uint32_t before = crc32c::Value(data.data(), data.size());
  data[2048] ^= 0x01;
  EXPECT_NE(crc32c::Value(data.data(), data.size()), before);
}

}  // namespace
}  // namespace ringstore
