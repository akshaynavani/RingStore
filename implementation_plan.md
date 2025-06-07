# RingStore — Implementation Plan

**RingStore** is an io_uring-based asynchronous block storage engine written in C++17.
It exposes a block-addressed read/write API backed by a file or raw block device, drives
all I/O through io_uring (with optional O_DIRECT, registered buffers and registered files),
ships a benchmark tool that sweeps queue depths and block sizes to measure IOPS / latency,
and is validated by Google Test suites.

## Key design decisions

| Decision | Choice | Why |
|---|---|---|
| io_uring access | `liburing` (>= 2.0) | Stable, well-documented helpers over the raw syscalls; avoids hand-rolling SQ/CQ ring memory barriers. |
| Build system | CMake >= 3.16 | Standard for C++; `FetchContent` pulls Google Test so no system install is needed. |
| Concurrency model | One ring per `IoEngine`, a single reaper thread drains the CQ; submitters are thread-safe behind a mutex + SQ-space semaphore. | Simple, correct, and still able to keep deep queues saturated. Per-thread rings are possible by creating more engines. |
| Completion API | Callback (`std::function<void(IoResult)>`) plus a `std::future` convenience wrapper. | Callbacks for the hot path (benchmarks), futures for ergonomic tests / sync callers. |
| Buffers | `AlignedBuffer` (posix_memalign, 4 KiB alignment) + `BufferPool` that can register its buffers with the ring (`IORING_OP_READ_FIXED`/`WRITE_FIXED`). | O_DIRECT needs aligned memory; fixed buffers save per-I/O page pinning. |
| Data integrity | `BlockStore` stores a superblock + per-block CRC32C in a separate checksum region. | Lets reads verify data without a slow read-modify-write on the data path. |
| Benchmark | Closed-loop generator keeping exactly `qd` requests in flight; random / sequential, read / write / mixed; HDR-style log2 latency histogram; CSV output. Baseline: synchronous `pread`/`pwrite`. | Matches "IOPS across queue depths and block sizes". |

Platform requirement: Linux kernel >= 5.6 (5.10+ recommended) with liburing installed.

---

## Phase 1 — Project skeleton & io_uring primitives

Deliverables
- CMake project with `ringstore` static library, warnings-as-errors option, liburing detection via pkg-config.
- `Status` / error helpers (errno -> message).
- `AlignedBuffer` RAII type and `BufferPool` (fixed-size, aligned, lock-protected free list).
- `Ring` RAII wrapper around `io_uring` (init with flags such as SQPOLL, get SQE, submit, wait/peek CQE, register buffers / files).
- `File` RAII wrapper (open with O_DIRECT optional, size, fallocate, fsync).

Files: `CMakeLists.txt`, `include/ringstore/{status,aligned_buffer,buffer_pool,ring,file}.h`,
`src/{status,aligned_buffer,buffer_pool,ring,file}.cpp`.

## Phase 2 — Async I/O engine

Deliverables
- `IoRequest` / `IoResult` types (op, fd, offset, buffer, length, user callback).
- `IoEngine`: submission path (thread-safe, back-pressure when SQ full, batched `io_uring_submit`),
  reaper thread draining CQEs and dispatching callbacks, short read/write resubmission,
  `fsync` op, graceful shutdown that drains in-flight requests.
- Fixed buffer and fixed file support (`READ_FIXED`/`WRITE_FIXED`, `IOSQE_FIXED_FILE`).
- Future-returning convenience APIs (`read_async`, `write_async`, `fsync_async`).
- Engine stats (submitted, completed, errors, in-flight high-water mark).

Files: `include/ringstore/{io_request,io_engine,engine_options}.h`, `src/io_engine.cpp`.

## Phase 3 — Block storage layer

Deliverables
- `crc32c` (software table implementation).
- On-disk layout: superblock (magic, version, block size, block count, checksum region offset) at block 0,
  checksum region, then data region.
- `BlockStore::create/open`, `read_block`, `write_block`, multi-block `read_blocks`/`write_blocks`,
  `flush`, all async via `IoEngine`, with CRC verification on read and checksum region persistence on flush.
- Bounds and alignment validation.

Files: `include/ringstore/{crc32c,layout,block_store}.h`, `src/{crc32c,layout,block_store}.cpp`.

## Phase 4 — Benchmark harness

Deliverables
- `LatencyHistogram` (log2 buckets, percentiles p50/p99/p999).
- Workload generator (seq/rand, read/write/mixed ratio, block size, queue depth, duration / op count).
- `ringstore_bench` CLI: sweep queue depths and block sizes, io_uring vs synchronous `pread`/`pwrite` baseline,
  table + CSV output.
- Helper script to sweep and plot results.

Files: `bench/{histogram.h,workload.h,workload.cpp,bench_main.cpp}`, `scripts/{run_sweep.sh,plot_results.py}`.

## Phase 5 — Google Test suites

Deliverables
- GoogleTest fetched via `FetchContent`, `ctest` integration.
- Tests: aligned buffer & pool, ring basics, engine read/write round-trip, concurrency stress
  (many threads, deep queues), short I/O / error propagation, CRC32C known vectors,
  BlockStore create/open/persist/corruption detection, histogram percentiles.

Files: `tests/CMakeLists.txt`, `tests/{test_buffer,test_ring,test_engine,test_crc32c,test_block_store,test_histogram}.cpp`, `tests/test_util.h`.

## Wrap-up
- `.gitignore` for CMake/C++ build outputs.
- `README.md` with architecture diagram (Mermaid), run and test instructions.
