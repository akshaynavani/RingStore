#pragma once

#include <cstdint>

#include "ringstore/status.h"

namespace ringstore {

// On-disk format (all integers little-endian, offsets in bytes):
//
//   +-------------------+  0
//   | superblock        |  one block, padded with zeros
//   +-------------------+  csum_offset = block_size
//   | checksum region   |  data_blocks * 4 bytes of CRC32C, rounded up to whole blocks
//   +-------------------+  data_offset
//   | data region       |  data_blocks * block_size
//   +-------------------+
//
// Checksums live apart from data so a block write is a single aligned I/O;
// the checksum region is persisted on Flush().

constexpr std::uint64_t kSuperblockMagic = 0x31525453474E4952ull;  // "RINGSTR1"
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uint32_t kMinBlockSize = 512;
constexpr std::uint32_t kMaxBlockSize = 1u << 20;

#pragma pack(push, 1)
struct Superblock {
  std::uint64_t magic;
  std::uint32_t version;
  std::uint32_t block_size;
  std::uint64_t data_blocks;
  std::uint64_t csum_offset;
  std::uint64_t csum_blocks;
  std::uint64_t data_offset;
  std::uint64_t created_unix_ns;
  std::uint32_t flags;
  std::uint32_t crc;  // CRC32C of this struct with `crc` set to 0
};
#pragma pack(pop)

static_assert(sizeof(Superblock) <= kMinBlockSize, "superblock must fit in the smallest block");

struct Layout {
  std::uint32_t block_size = 0;
  std::uint64_t data_blocks = 0;
  std::uint64_t csum_offset = 0;
  std::uint64_t csum_blocks = 0;
  std::uint64_t data_offset = 0;

  static Status Compute(std::uint32_t block_size, std::uint64_t data_blocks, Layout* out);

  std::uint64_t total_bytes() const { return data_offset + data_blocks * block_size; }
  std::uint64_t data_block_offset(std::uint64_t block) const {
    return data_offset + block * block_size;
  }
  // Checksums per checksum-region block.
  std::uint64_t csums_per_block() const { return block_size / sizeof(std::uint32_t); }
};

Superblock MakeSuperblock(const Layout& layout, std::uint64_t created_unix_ns);
// Validates magic, version, CRC and internal consistency, then fills `layout`.
Status ParseSuperblock(const void* block, std::uint32_t expected_block_size_hint, Layout* layout);

}  // namespace ringstore
