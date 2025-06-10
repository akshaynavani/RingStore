#include "ringstore/layout.h"

#include <cstring>
#include <string>

#include "ringstore/crc32c.h"

namespace ringstore {

namespace {

bool IsPowerOfTwo(std::uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }

std::uint32_t SuperblockCrc(Superblock sb) {
  sb.crc = 0;
  return crc32c::Value(&sb, sizeof(sb));
}

}  // namespace

Status Layout::Compute(std::uint32_t block_size, std::uint64_t data_blocks, Layout* out) {
  if (!IsPowerOfTwo(block_size) || block_size < kMinBlockSize || block_size > kMaxBlockSize) {
    return Status::InvalidArgument("block size must be a power of two in [512, 1MiB]");
  }
  if (data_blocks == 0) return Status::InvalidArgument("store must have at least one block");

  Layout l;
  l.block_size = block_size;
  l.data_blocks = data_blocks;
  l.csum_offset = block_size;
  const std::uint64_t csum_bytes = data_blocks * sizeof(std::uint32_t);
  l.csum_blocks = (csum_bytes + block_size - 1) / block_size;
  l.data_offset = l.csum_offset + l.csum_blocks * block_size;
  *out = l;
  return Status::Ok();
}

Superblock MakeSuperblock(const Layout& layout, std::uint64_t created_unix_ns) {
  Superblock sb;
  std::memset(&sb, 0, sizeof(sb));
  sb.magic = kSuperblockMagic;
  sb.version = kFormatVersion;
  sb.block_size = layout.block_size;
  sb.data_blocks = layout.data_blocks;
  sb.csum_offset = layout.csum_offset;
  sb.csum_blocks = layout.csum_blocks;
  sb.data_offset = layout.data_offset;
  sb.created_unix_ns = created_unix_ns;
  sb.flags = 0;
  sb.crc = SuperblockCrc(sb);
  return sb;
}

Status ParseSuperblock(const void* block, std::uint32_t expected_block_size_hint, Layout* layout) {
  Superblock sb;
  std::memcpy(&sb, block, sizeof(sb));
  if (sb.magic != kSuperblockMagic) return Status::Corruption("bad superblock magic");
  if (sb.version != kFormatVersion) {
    return Status::Corruption("unsupported format version " + std::to_string(sb.version));
  }
  if (SuperblockCrc(sb) != sb.crc) return Status::Corruption("superblock checksum mismatch");
  if (expected_block_size_hint != 0 && sb.block_size != expected_block_size_hint) {
    return Status::InvalidArgument("block size mismatch: store uses " +
                                   std::to_string(sb.block_size));
  }

  Layout expected;
  Status s = Layout::Compute(sb.block_size, sb.data_blocks, &expected);
  if (!s.ok()) return Status::Corruption("superblock geometry invalid: " + s.message());
  if (expected.csum_offset != sb.csum_offset || expected.csum_blocks != sb.csum_blocks ||
      expected.data_offset != sb.data_offset) {
    return Status::Corruption("superblock region offsets inconsistent");
  }
  *layout = expected;
  return Status::Ok();
}

}  // namespace ringstore
