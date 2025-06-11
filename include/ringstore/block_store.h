#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ringstore/file.h"
#include "ringstore/io_engine.h"
#include "ringstore/layout.h"
#include "ringstore/status.h"

namespace ringstore {

struct BlockStoreOptions {
  std::uint32_t block_size = 4096;
  // Number of data blocks (Create only; Open reads it from the superblock).
  std::uint64_t num_blocks = 0;
  // Open the backing file with O_DIRECT. Buffers must then be aligned to the
  // device's logical block size (AlignedBuffer / BufferPool guarantee 4 KiB).
  bool direct_io = true;
  // Verify CRC32C on every read.
  bool verify_checksums = true;
};

using BlockCallback = std::function<void(const Status&)>;

// Fixed-size block store over a file or raw block device, with all data-path
// I/O issued through an IoEngine.
//
// Semantics:
//  * Every block carries a CRC32C kept in memory and persisted to the
//    checksum region by Flush(). Blocks never written since Create() have no
//    checksum and are returned unverified.
//  * A multi-block read/write is a single I/O to a contiguous range.
//  * Flush() makes durable every write whose callback has already fired.
//  * Concurrent writes (or a read racing a write) to the same block are not
//    ordered; callers must serialise them.
class BlockStore {
 public:
  ~BlockStore();

  BlockStore(const BlockStore&) = delete;
  BlockStore& operator=(const BlockStore&) = delete;

  // Formats `path` (created/truncated if it is a regular file).
  static Status Create(const std::string& path, const BlockStoreOptions& options,
                       IoEngine* engine, std::unique_ptr<BlockStore>* out);
  // Opens an existing store. options.block_size, if non-zero, must match.
  static Status Open(const std::string& path, const BlockStoreOptions& options,
                     IoEngine* engine, std::unique_ptr<BlockStore>* out);

  // Asynchronous API. On a non-OK return the callback is never invoked.
  Status ReadBlocksAsync(std::uint64_t first, std::uint32_t count, std::uint8_t* buf,
                         BlockCallback cb);
  Status WriteBlocksAsync(std::uint64_t first, std::uint32_t count, const std::uint8_t* buf,
                          BlockCallback cb);
  Status ReadBlockAsync(std::uint64_t block, std::uint8_t* buf, BlockCallback cb) {
    return ReadBlocksAsync(block, 1, buf, std::move(cb));
  }
  Status WriteBlockAsync(std::uint64_t block, const std::uint8_t* buf, BlockCallback cb) {
    return WriteBlocksAsync(block, 1, buf, std::move(cb));
  }

  // Blocking API.
  Status ReadBlocks(std::uint64_t first, std::uint32_t count, std::uint8_t* buf);
  Status WriteBlocks(std::uint64_t first, std::uint32_t count, const std::uint8_t* buf);
  Status ReadBlock(std::uint64_t block, std::uint8_t* buf) { return ReadBlocks(block, 1, buf); }
  Status WriteBlock(std::uint64_t block, const std::uint8_t* buf) {
    return WriteBlocks(block, 1, buf);
  }

  // Persists dirty checksum blocks, then fdatasyncs the file.
  Status Flush();
  // Flush() + close. Safe to call more than once.
  Status Close();

  std::uint32_t block_size() const { return layout_.block_size; }
  std::uint64_t num_blocks() const { return layout_.data_blocks; }
  const Layout& layout() const { return layout_; }
  const File& file() const { return file_; }
  std::uint64_t checksum_failures() const { return checksum_failures_.load(); }

 private:
  BlockStore(IoEngine* engine, File file, const Layout& layout, bool verify);

  Status CheckArgs(std::uint64_t first, std::uint32_t count, const std::uint8_t* buf) const;
  static std::uint32_t EncodeCrc(std::uint32_t crc) { return crc == 0 ? 1u : crc; }

  // Synchronous whole-region helpers used by Create/Open/Flush.
  Status WriteChecksumBlocks(std::uint64_t first_cblock, std::uint64_t count,
                             const std::uint8_t* src);
  Status LoadChecksums();
  Status RunSync(const std::function<Status(BlockCallback)>& start);

  IoEngine* engine_;
  File file_;
  Layout layout_;
  bool verify_;
  bool closed_ = false;

  std::mutex csum_mu_;
  // Indexed by data block; 0 means "never written".
  std::vector<std::uint32_t> csums_;
  // Indexed by checksum-region block.
  std::vector<bool> dirty_;

  std::atomic<std::uint64_t> checksum_failures_{0};
};

}  // namespace ringstore
