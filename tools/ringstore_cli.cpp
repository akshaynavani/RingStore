// ringstore_cli: format, inspect and scrub a RingStore file or block device.
//
//   ringstore_cli format <path> <num_blocks> [block_size]
//   ringstore_cli info   <path>
//   ringstore_cli fill   <path> <seed>      # write a deterministic pattern to every block
//   ringstore_cli verify <path> <seed>      # read back and compare against the pattern
//   ringstore_cli scrub  <path>             # read every block and verify checksums only

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "ringstore/aligned_buffer.h"
#include "ringstore/block_store.h"
#include "ringstore/io_engine.h"

using namespace ringstore;

namespace {

constexpr std::uint32_t kBatchBlocks = 64;

void FillPattern(std::uint8_t* buf, std::uint32_t bs, std::uint64_t block, std::uint64_t seed) {
  std::uint64_t x = seed ^ (block * 0x9E3779B97F4A7C15ull);
  for (std::uint32_t i = 0; i < bs; i += 8) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    std::memcpy(buf + i, &x, 8);
  }
}

int Usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  ringstore_cli format <path> <num_blocks> [block_size]\n"
               "  ringstore_cli info   <path>\n"
               "  ringstore_cli fill   <path> <seed>\n"
               "  ringstore_cli verify <path> <seed>\n"
               "  ringstore_cli scrub  <path>\n");
  return 2;
}

int Fail(const char* what, const Status& s) {
  std::fprintf(stderr, "%s: %s\n", what, s.ToString().c_str());
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return Usage();
  const std::string cmd = argv[1];
  const std::string path = argv[2];

  IoEngine engine;
  EngineOptions eopts;
  eopts.ring.entries = 128;
  Status s = engine.Start(eopts);
  if (!s.ok()) return Fail("engine start", s);

  std::unique_ptr<BlockStore> store;
  if (cmd == "format") {
    if (argc < 4) return Usage();
    BlockStoreOptions opts;
    opts.num_blocks = std::strtoull(argv[3], nullptr, 10);
    if (argc > 4) opts.block_size = static_cast<std::uint32_t>(std::strtoul(argv[4], nullptr, 10));
    s = BlockStore::Create(path, opts, &engine, &store);
    if (!s.ok()) return Fail("format", s);
    std::printf("formatted %s: %llu blocks x %u bytes\n", path.c_str(),
                static_cast<unsigned long long>(store->num_blocks()), store->block_size());
    return store->Close().ok() ? 0 : 1;
  }

  BlockStoreOptions opts;
  opts.block_size = 0;  // accept whatever the superblock says
  s = BlockStore::Open(path, opts, &engine, &store);
  if (!s.ok()) return Fail("open", s);
  const Layout& l = store->layout();
  const std::uint32_t bs = l.block_size;

  if (cmd == "info") {
    std::printf("block_size   %u\n", bs);
    std::printf("data_blocks  %llu\n", static_cast<unsigned long long>(l.data_blocks));
    std::printf("csum_offset  %llu (%llu blocks)\n", static_cast<unsigned long long>(l.csum_offset),
                static_cast<unsigned long long>(l.csum_blocks));
    std::printf("data_offset  %llu\n", static_cast<unsigned long long>(l.data_offset));
    std::printf("total_bytes  %llu\n", static_cast<unsigned long long>(l.total_bytes()));
    std::printf("block_device %s, O_DIRECT %s\n", store->file().is_block_device() ? "yes" : "no",
                store->file().is_direct() ? "yes" : "no");
    return 0;
  }

  const bool fill = cmd == "fill";
  const bool verify = cmd == "verify";
  const bool scrub = cmd == "scrub";
  if (!fill && !verify && !scrub) return Usage();
  if ((fill || verify) && argc < 4) return Usage();
  const std::uint64_t seed = (fill || verify) ? std::strtoull(argv[3], nullptr, 10) : 0;

  AlignedBuffer buf(static_cast<std::size_t>(kBatchBlocks) * bs);
  AlignedBuffer expect(bs);
  std::uint64_t bad = 0;
  for (std::uint64_t b = 0; b < l.data_blocks; b += kBatchBlocks) {
    auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(kBatchBlocks, l.data_blocks - b));
    if (fill) {
      for (std::uint32_t i = 0; i < n; ++i) FillPattern(buf.data() + std::size_t{i} * bs, bs, b + i, seed);
      s = store->WriteBlocks(b, n, buf.data());
      if (!s.ok()) return Fail("write", s);
      continue;
    }
    s = store->ReadBlocks(b, n, buf.data());
    if (!s.ok()) {
      std::fprintf(stderr, "blocks %llu..%llu: %s\n", static_cast<unsigned long long>(b),
                   static_cast<unsigned long long>(b + n - 1), s.ToString().c_str());
      ++bad;
      continue;
    }
    if (verify) {
      for (std::uint32_t i = 0; i < n; ++i) {
        FillPattern(expect.data(), bs, b + i, seed);
        if (std::memcmp(expect.data(), buf.data() + std::size_t{i} * bs, bs) != 0) {
          std::fprintf(stderr, "block %llu: content mismatch\n",
                       static_cast<unsigned long long>(b + i));
          ++bad;
        }
      }
    }
  }
  s = store->Close();
  if (!s.ok()) return Fail("close", s);
  std::printf("%s complete: %llu blocks, %llu problems\n", cmd.c_str(),
              static_cast<unsigned long long>(l.data_blocks), static_cast<unsigned long long>(bad));
  return bad == 0 ? 0 : 1;
}
