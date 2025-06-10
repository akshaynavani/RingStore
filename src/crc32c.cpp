#include "ringstore/crc32c.h"

#include <array>
#include <cstring>

namespace ringstore {
namespace crc32c {

namespace {

constexpr std::uint32_t kPoly = 0x82F63B78u;

// Slice-by-8 tables: table[0] is the classic byte table, table[k] advances a
// byte that sits k positions further back. Processing 8 bytes per step is
// several times faster than the byte-at-a-time loop.
struct Tables {
  std::array<std::array<std::uint32_t, 256>, 8> t{};
  Tables() {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ kPoly : (c >> 1);
      t[0][i] = c;
    }
    for (std::uint32_t i = 0; i < 256; ++i) {
      for (std::size_t k = 1; k < 8; ++k) {
        t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 0xFF];
      }
    }
  }
};

const Tables& GetTables() {
  static const Tables tables;
  return tables;
}

}  // namespace

std::uint32_t Extend(std::uint32_t crc, const void* data, std::size_t n) {
  const auto& t = GetTables().t;
  const auto* p = static_cast<const std::uint8_t*>(data);
  std::uint32_t c = ~crc;

  while (n >= 8) {
    std::uint32_t lo;
    std::uint32_t hi;
    std::memcpy(&lo, p, 4);
    std::memcpy(&hi, p + 4, 4);
    lo ^= c;  // little-endian host assumed (x86-64 / aarch64 Linux)
    c = t[7][lo & 0xFF] ^ t[6][(lo >> 8) & 0xFF] ^ t[5][(lo >> 16) & 0xFF] ^ t[4][lo >> 24] ^
        t[3][hi & 0xFF] ^ t[2][(hi >> 8) & 0xFF] ^ t[1][(hi >> 16) & 0xFF] ^ t[0][hi >> 24];
    p += 8;
    n -= 8;
  }
  while (n-- > 0) {
    c = t[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
  }
  return ~c;
}

}  // namespace crc32c
}  // namespace ringstore
