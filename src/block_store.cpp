#include "ringstore/block_store.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <future>
#include <limits>
#include <string>

#include "ringstore/aligned_buffer.h"
#include "ringstore/crc32c.h"

namespace ringstore {

namespace {

constexpr std::uint64_t kMetaChunkBytes = 1u << 20;  // checksum region I/O size

std::uint64_t UnixNowNs() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count());
}

Status ResultToStatus(const IoResult& r, std::uint32_t expected) {
  if (!r.status.ok()) return r.status;
  if (r.bytes != expected) {
    return Status::IoError("short transfer: " + std::to_string(r.bytes) + " of " +
                           std::to_string(expected) + " bytes");
  }
  return Status::Ok();
}

}  // namespace

BlockStore::BlockStore(IoEngine* engine, File file, const Layout& layout, bool verify)
    : engine_(engine), file_(std::move(file)), layout_(layout), verify_(verify) {
  csums_.assign(layout_.csum_blocks * layout_.csums_per_block(), 0);
  dirty_.assign(layout_.csum_blocks, false);
}

BlockStore::~BlockStore() { Close(); }

Status BlockStore::Create(const std::string& path, const BlockStoreOptions& options,
                          IoEngine* engine, std::unique_ptr<BlockStore>* out) {
  if (engine == nullptr || !engine->running()) {
    return Status::InvalidArgument("a running IoEngine is required");
  }
  Layout layout;
  Status s = Layout::Compute(options.block_size, options.num_blocks, &layout);
  if (!s.ok()) return s;

  FileOptions fopts;
  fopts.create = true;
  fopts.direct = options.direct_io;
  File file;
  s = File::Open(path, fopts, &file);
  if (!s.ok()) return s;
  if (!file.is_block_device()) {
    // Start from an empty file so stale bytes from an old store never leak.
    if (::ftruncate(file.fd(), 0) != 0) return Status::FromErrno("ftruncate", errno);
  }
  if (layout.block_size < file.logical_block_size()) {
    return Status::InvalidArgument("block size smaller than device logical block size");
  }
  s = file.Allocate(layout.total_bytes());
  if (!s.ok()) return s;

  std::unique_ptr<BlockStore> store(
      new BlockStore(engine, std::move(file), layout, options.verify_checksums));

  // Zeroed checksum region: every block starts as "never written".
  AlignedBuffer zeros(std::min<std::uint64_t>(kMetaChunkBytes, layout.csum_blocks * layout.block_size));
  for (std::uint64_t cb = 0; cb < layout.csum_blocks;) {
    std::uint64_t n = std::min<std::uint64_t>(zeros.size() / layout.block_size, layout.csum_blocks - cb);
    s = store->WriteChecksumBlocks(cb, n, zeros.data());
    if (!s.ok()) return s;
    cb += n;
  }

  // The superblock goes last so a crash mid-format leaves an unopenable store
  // rather than one with a garbage checksum region.
  IoResult r = engine->Fsync(store->file_.fd(), true);
  if (!r.status.ok()) return r.status;
  AlignedBuffer sb_block(layout.block_size);
  Superblock sb = MakeSuperblock(layout, UnixNowNs());
  std::memcpy(sb_block.data(), &sb, sizeof(sb));
  r = engine->Write(store->file_.fd(), sb_block.data(), layout.block_size, 0);
  s = ResultToStatus(r, layout.block_size);
  if (!s.ok()) return s;
  r = engine->Fsync(store->file_.fd(), true);
  if (!r.status.ok()) return r.status;

  *out = std::move(store);
  return Status::Ok();
}

Status BlockStore::Open(const std::string& path, const BlockStoreOptions& options,
                        IoEngine* engine, std::unique_ptr<BlockStore>* out) {
  if (engine == nullptr || !engine->running()) {
    return Status::InvalidArgument("a running IoEngine is required");
  }
  FileOptions fopts;
  fopts.direct = options.direct_io;
  File file;
  Status s = File::Open(path, fopts, &file);
  if (!s.ok()) return s;

  // The superblock fits in the first 512 bytes, but O_DIRECT needs reads
  // aligned to the device's logical block size.
  const std::uint32_t probe = std::max<std::uint32_t>(kMinBlockSize, file.logical_block_size());
  AlignedBuffer head(probe);
  IoResult r = engine->Read(file.fd(), head.data(), probe, 0);
  if (!r.status.ok()) return r.status;
  if (r.bytes < sizeof(Superblock)) return Status::Corruption("file too small for a superblock");

  Layout layout;
  s = ParseSuperblock(head.data(), options.block_size, &layout);
  if (!s.ok()) return s;

  std::uint64_t size = 0;
  s = file.Size(&size);
  if (!s.ok()) return s;
  if (size < layout.total_bytes()) {
    return Status::Corruption("backing file is smaller than the store geometry");
  }

  std::unique_ptr<BlockStore> store(
      new BlockStore(engine, std::move(file), layout, options.verify_checksums));
  s = store->LoadChecksums();
  if (!s.ok()) return s;
  *out = std::move(store);
  return Status::Ok();
}

Status BlockStore::CheckArgs(std::uint64_t first, std::uint32_t count,
                             const std::uint8_t* buf) const {
  if (closed_) return Status::InvalidArgument("store is closed");
  if (buf == nullptr) return Status::InvalidArgument("null buffer");
  if (count == 0) return Status::InvalidArgument("count must be > 0");
  if (first >= layout_.data_blocks || count > layout_.data_blocks - first) {
    return Status::OutOfRange("block range [" + std::to_string(first) + ", " +
                              std::to_string(first + count) + ") exceeds " +
                              std::to_string(layout_.data_blocks) + " blocks");
  }
  if (static_cast<std::uint64_t>(count) * layout_.block_size >
      std::numeric_limits<std::uint32_t>::max()) {
    return Status::InvalidArgument("I/O larger than 4 GiB");
  }
  if (file_.is_direct() &&
      reinterpret_cast<std::uintptr_t>(buf) % file_.logical_block_size() != 0) {
    return Status::InvalidArgument("O_DIRECT buffer is not aligned to the logical block size");
  }
  return Status::Ok();
}

Status BlockStore::ReadBlocksAsync(std::uint64_t first, std::uint32_t count, std::uint8_t* buf,
                                   BlockCallback cb) {
  Status s = CheckArgs(first, count, buf);
  if (!s.ok()) return s;
  const std::uint32_t bs = layout_.block_size;
  const std::uint32_t len = count * bs;

  auto on_done = [this, first, count, buf, bs, len, cb = std::move(cb)](const IoResult& r) {
    Status st = ResultToStatus(r, len);
    if (st.ok() && verify_) {
      std::lock_guard<std::mutex> lock(csum_mu_);
      for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t expected = csums_[first + i];
        if (expected == 0) continue;  // never written
        std::uint32_t actual = EncodeCrc(crc32c::Value(buf + static_cast<std::size_t>(i) * bs, bs));
        if (actual != expected) {
          checksum_failures_.fetch_add(1);
          st = Status::Corruption("checksum mismatch on block " + std::to_string(first + i));
          break;
        }
      }
    }
    if (cb) cb(st);
  };
  return engine_->Submit(
      IoRequest::Read(file_.fd(), buf, len, layout_.data_block_offset(first), std::move(on_done)));
}

Status BlockStore::WriteBlocksAsync(std::uint64_t first, std::uint32_t count,
                                    const std::uint8_t* buf, BlockCallback cb) {
  Status s = CheckArgs(first, count, buf);
  if (!s.ok()) return s;
  const std::uint32_t bs = layout_.block_size;
  const std::uint32_t len = count * bs;

  // Checksum before submission: the caller must keep the buffer stable until
  // the callback fires anyway, and this keeps CRC work off the reaper thread.
  std::vector<std::uint32_t> crcs(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    crcs[i] = EncodeCrc(crc32c::Value(buf + static_cast<std::size_t>(i) * bs, bs));
  }

  auto on_done = [this, first, len, crcs = std::move(crcs), cb = std::move(cb)](const IoResult& r) {
    Status st = ResultToStatus(r, len);
    if (st.ok()) {
      std::lock_guard<std::mutex> lock(csum_mu_);
      const std::uint64_t per = layout_.csums_per_block();
      for (std::size_t i = 0; i < crcs.size(); ++i) {
        csums_[first + i] = crcs[i];
        dirty_[(first + i) / per] = true;
      }
    }
    if (cb) cb(st);
  };
  return engine_->Submit(IoRequest::Write(file_.fd(), const_cast<std::uint8_t*>(buf), len,
                                          layout_.data_block_offset(first), std::move(on_done)));
}

Status BlockStore::RunSync(const std::function<Status(BlockCallback)>& start) {
  auto promise = std::make_shared<std::promise<Status>>();
  auto future = promise->get_future();
  Status s = start([promise](const Status& st) { promise->set_value(st); });
  if (!s.ok()) return s;
  return future.get();
}

Status BlockStore::ReadBlocks(std::uint64_t first, std::uint32_t count, std::uint8_t* buf) {
  return RunSync([&](BlockCallback cb) { return ReadBlocksAsync(first, count, buf, std::move(cb)); });
}

Status BlockStore::WriteBlocks(std::uint64_t first, std::uint32_t count,
                               const std::uint8_t* buf) {
  return RunSync(
      [&](BlockCallback cb) { return WriteBlocksAsync(first, count, buf, std::move(cb)); });
}

Status BlockStore::WriteChecksumBlocks(std::uint64_t first_cblock, std::uint64_t count,
                                       const std::uint8_t* src) {
  const std::uint64_t bs = layout_.block_size;
  // `src` may be an unaligned view into csums_, so stage through aligned memory.
  const std::uint64_t chunk_blocks = std::max<std::uint64_t>(1, kMetaChunkBytes / bs);
  AlignedBuffer staging(std::min(count, chunk_blocks) * bs);
  for (std::uint64_t done = 0; done < count;) {
    std::uint64_t n = std::min(chunk_blocks, count - done);
    std::uint32_t len = static_cast<std::uint32_t>(n * bs);
    std::memcpy(staging.data(), src + done * bs, len);
    IoResult r = engine_->Write(file_.fd(), staging.data(), len,
                                layout_.csum_offset + (first_cblock + done) * bs);
    Status s = ResultToStatus(r, len);
    if (!s.ok()) return s;
    done += n;
  }
  return Status::Ok();
}

Status BlockStore::LoadChecksums() {
  const std::uint64_t bs = layout_.block_size;
  const std::uint64_t chunk_blocks = std::max<std::uint64_t>(1, kMetaChunkBytes / bs);
  AlignedBuffer staging(std::min(layout_.csum_blocks, chunk_blocks) * bs);
  auto* dst = reinterpret_cast<std::uint8_t*>(csums_.data());
  for (std::uint64_t done = 0; done < layout_.csum_blocks;) {
    std::uint64_t n = std::min(chunk_blocks, layout_.csum_blocks - done);
    std::uint32_t len = static_cast<std::uint32_t>(n * bs);
    IoResult r = engine_->Read(file_.fd(), staging.data(), len, layout_.csum_offset + done * bs);
    Status s = ResultToStatus(r, len);
    if (!s.ok()) return s;
    std::memcpy(dst + done * bs, staging.data(), len);
    done += n;
  }
  return Status::Ok();
}

Status BlockStore::Flush() {
  if (closed_) return Status::InvalidArgument("store is closed");
  const std::uint64_t bs = layout_.block_size;
  const std::uint64_t per = layout_.csums_per_block();

  // Snapshot dirty checksum blocks as contiguous runs.
  struct Run {
    std::uint64_t first;
    std::vector<std::uint8_t> bytes;
  };
  std::vector<Run> runs;
  {
    std::lock_guard<std::mutex> lock(csum_mu_);
    for (std::uint64_t cb = 0; cb < dirty_.size();) {
      if (!dirty_[cb]) {
        ++cb;
        continue;
      }
      std::uint64_t end = cb;
      while (end < dirty_.size() && dirty_[end]) dirty_[end++] = false;
      Run run;
      run.first = cb;
      const auto* src = reinterpret_cast<const std::uint8_t*>(csums_.data() + cb * per);
      run.bytes.assign(src, src + (end - cb) * bs);
      runs.push_back(std::move(run));
      cb = end;
    }
  }

  for (std::size_t i = 0; i < runs.size(); ++i) {
    const Run& run = runs[i];
    Status s = WriteChecksumBlocks(run.first, run.bytes.size() / bs, run.bytes.data());
    if (!s.ok()) {
      // Re-mark everything not yet persisted so a later Flush retries it.
      std::lock_guard<std::mutex> lock(csum_mu_);
      for (std::size_t j = i; j < runs.size(); ++j) {
        for (std::uint64_t k = 0; k < runs[j].bytes.size() / bs; ++k) dirty_[runs[j].first + k] = true;
      }
      return s;
    }
  }

  IoResult r = engine_->Fsync(file_.fd(), true);
  return r.status;
}

Status BlockStore::Close() {
  if (closed_) return Status::Ok();
  Status s = Status::Ok();
  if (engine_ != nullptr && engine_->running()) s = Flush();
  closed_ = true;
  Status c = file_.Close();
  return s.ok() ? c : s;
}

}  // namespace ringstore
