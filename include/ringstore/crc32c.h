#pragma once

#include <cstddef>
#include <cstdint>

namespace ringstore {
namespace crc32c {

// CRC-32C (Castagnoli, reflected polynomial 0x82F63B78) as used by iSCSI,
// ext4 and RocksDB. `Extend` continues a running CRC over more data.
std::uint32_t Extend(std::uint32_t crc, const void* data, std::size_t n);

inline std::uint32_t Value(const void* data, std::size_t n) { return Extend(0, data, n); }

}  // namespace crc32c
}  // namespace ringstore
