// ringstore_bench: sweep queue depths and block sizes, measuring IOPS,
// throughput and latency percentiles for io_uring vs. synchronous pread/pwrite.
//
// Example:
//   ringstore_bench --file /mnt/nvme/rs.dat --size 4G --bs 4k,16k,64k \
//       --qd 1,4,16,64,128 --rw randread --time 10 --engine both --csv out.csv

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "workload.h"

using namespace ringstore;
using namespace ringstore::bench;

namespace {

struct Options {
  WorkloadSpec base;
  std::vector<std::uint32_t> block_sizes{4096};
  std::vector<unsigned> queue_depths{1, 4, 16, 64};
  bool run_uring = true;
  bool run_sync = false;
  bool prepare = true;
  std::string csv_path;
};

void PrintUsage() {
  std::fprintf(stderr,
               "usage: ringstore_bench --file PATH [options]\n"
               "  --size N[k|m|g]        target size (default 1g)\n"
               "  --bs LIST              block sizes, e.g. 4k,16k,128k (default 4k)\n"
               "  --qd LIST              queue depths, e.g. 1,4,16,64 (default 1,4,16,64)\n"
               "  --rw MODE              randread|randwrite|read|write|randrw[:READPCT] (default randread)\n"
               "  --time SECONDS         measured duration per point (default 10)\n"
               "  --warmup SECONDS       warmup per point, excluded (default 1)\n"
               "  --ops N                stop after N ops (default: time-bound)\n"
               "  --engine uring|sync|both  (default uring)\n"
               "  --buffered             do not use O_DIRECT\n"
               "  --fixed-bufs           use registered buffers (READ/WRITE_FIXED)\n"
               "  --fixed-files          use registered file descriptors\n"
               "  --sqpoll               kernel-side SQ polling thread\n"
               "  --no-prepare           skip pre-filling the target file\n"
               "  --seed N               RNG seed (default 42)\n"
               "  --csv PATH             append results as CSV\n");
}

std::uint64_t ParseSize(const std::string& s) {
  if (s.empty()) return 0;
  char* end = nullptr;
  double v = std::strtod(s.c_str(), &end);
  std::uint64_t mult = 1;
  switch (end != nullptr ? *end : '\0') {
    case 'k': case 'K': mult = 1ull << 10; break;
    case 'm': case 'M': mult = 1ull << 20; break;
    case 'g': case 'G': mult = 1ull << 30; break;
    case 't': case 'T': mult = 1ull << 40; break;
    default: break;
  }
  return static_cast<std::uint64_t>(v * static_cast<double>(mult));
}

template <typename T, typename F>
std::vector<T> ParseList(const std::string& s, F parse) {
  std::vector<T> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty()) out.push_back(static_cast<T>(parse(item)));
  }
  return out;
}

bool ParseRw(const std::string& mode, WorkloadSpec* spec) {
  std::string m = mode;
  int pct = -1;
  auto colon = m.find(':');
  if (colon != std::string::npos) {
    pct = std::atoi(m.c_str() + colon + 1);
    m = m.substr(0, colon);
  }
  if (m == "randread") { spec->pattern = Pattern::kRandom; spec->read_pct = 100; }
  else if (m == "randwrite") { spec->pattern = Pattern::kRandom; spec->read_pct = 0; }
  else if (m == "read") { spec->pattern = Pattern::kSequential; spec->read_pct = 100; }
  else if (m == "write") { spec->pattern = Pattern::kSequential; spec->read_pct = 0; }
  else if (m == "randrw") { spec->pattern = Pattern::kRandom; spec->read_pct = pct >= 0 ? pct : 70; }
  else if (m == "rw") { spec->pattern = Pattern::kSequential; spec->read_pct = pct >= 0 ? pct : 70; }
  else return false;
  return true;
}

bool ParseArgs(int argc, char** argv, Options* o) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--file") o->base.path = next();
    else if (a == "--size") o->base.file_size = ParseSize(next());
    else if (a == "--bs") o->block_sizes = ParseList<std::uint32_t>(next(), ParseSize);
    else if (a == "--qd") o->queue_depths = ParseList<unsigned>(next(), [](const std::string& x) { return std::strtoul(x.c_str(), nullptr, 10); });
    else if (a == "--rw") { if (!ParseRw(next(), &o->base)) return false; }
    else if (a == "--time") o->base.duration_s = std::atof(next().c_str());
    else if (a == "--warmup") o->base.warmup_s = std::atof(next().c_str());
    else if (a == "--ops") o->base.max_ops = std::strtoull(next().c_str(), nullptr, 10);
    else if (a == "--engine") {
      std::string e = next();
      o->run_uring = e == "uring" || e == "both";
      o->run_sync = e == "sync" || e == "both";
      if (!o->run_uring && !o->run_sync) return false;
    }
    else if (a == "--buffered") o->base.direct = false;
    else if (a == "--fixed-bufs") o->base.fixed_buffers = true;
    else if (a == "--fixed-files") o->base.fixed_files = true;
    else if (a == "--sqpoll") o->base.sqpoll = true;
    else if (a == "--no-prepare") o->prepare = false;
    else if (a == "--seed") o->base.seed = std::strtoull(next().c_str(), nullptr, 10);
    else if (a == "--csv") o->csv_path = next();
    else if (a == "-h" || a == "--help") return false;
    else {
      std::fprintf(stderr, "unknown option %s\n", a.c_str());
      return false;
    }
  }
  return !o->base.path.empty() && !o->block_sizes.empty() && !o->queue_depths.empty();
}

std::string RwLabel(const WorkloadSpec& s) {
  std::string p = PatternName(s.pattern);
  if (s.read_pct == 100) return p + "read";
  if (s.read_pct == 0) return p + "write";
  return p + "rw" + std::to_string(s.read_pct);
}

void PrintHeader() {
  std::printf("%-9s %-10s %7s %5s %12s %10s %9s %9s %9s %9s %8s %7s\n", "engine", "workload",
              "bs", "qd", "iops", "MiB/s", "avg_us", "p50_us", "p99_us", "p999_us", "sys/op",
              "errors");
}

void PrintRow(const WorkloadSpec& s, const WorkloadResult& r) {
  const auto& h = r.latency;
  double sys_per_op = r.ops > 0 ? static_cast<double>(r.submit_syscalls) / static_cast<double>(r.ops) : 0.0;
  std::printf("%-9s %-10s %7u %5u %12.0f %10.1f %9.1f %9.1f %9.1f %9.1f %8.3f %7llu\n",
              EngineName(r.engine), RwLabel(s).c_str(), s.block_size, s.queue_depth, r.iops(),
              r.mib_per_s(), h.mean() / 1e3, static_cast<double>(h.Percentile(0.50)) / 1e3,
              static_cast<double>(h.Percentile(0.99)) / 1e3,
              static_cast<double>(h.Percentile(0.999)) / 1e3, sys_per_op,
              static_cast<unsigned long long>(r.errors));
  std::fflush(stdout);
}

void AppendCsv(std::ofstream& csv, const WorkloadSpec& s, const WorkloadResult& r) {
  const auto& h = r.latency;
  csv << EngineName(r.engine) << ',' << RwLabel(s) << ',' << s.block_size << ',' << s.queue_depth
      << ',' << (s.direct ? 1 : 0) << ',' << (s.fixed_buffers ? 1 : 0) << ','
      << (s.fixed_files ? 1 : 0) << ',' << (s.sqpoll ? 1 : 0) << ',' << r.ops << ','
      << r.seconds << ',' << r.iops() << ',' << r.mib_per_s() << ',' << h.mean() / 1e3 << ','
      << static_cast<double>(h.Percentile(0.50)) / 1e3 << ','
      << static_cast<double>(h.Percentile(0.99)) / 1e3 << ','
      << static_cast<double>(h.Percentile(0.999)) / 1e3 << ','
      << static_cast<double>(h.max()) / 1e3 << ',' << r.submit_syscalls << ',' << r.errors
      << '\n';
  csv.flush();
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  opts.base.read_pct = 100;
  if (!ParseArgs(argc, argv, &opts)) {
    PrintUsage();
    return 2;
  }

  if (opts.prepare) {
    std::fprintf(stderr, "preparing %s (%llu bytes)...\n", opts.base.path.c_str(),
                 static_cast<unsigned long long>(opts.base.file_size));
    Status s = PrepareTarget(opts.base);
    if (!s.ok()) {
      std::fprintf(stderr, "prepare failed: %s\n", s.ToString().c_str());
      return 1;
    }
  }

  std::ofstream csv;
  if (!opts.csv_path.empty()) {
    std::ifstream exists(opts.csv_path);
    bool write_header = !exists.good() || exists.peek() == std::ifstream::traits_type::eof();
    csv.open(opts.csv_path, std::ios::app);
    if (write_header) {
      csv << "engine,workload,bs,qd,direct,fixed_bufs,fixed_files,sqpoll,ops,seconds,iops,mib_s,"
             "avg_us,p50_us,p99_us,p999_us,max_us,syscalls,errors\n";
    }
  }

  PrintHeader();
  int failures = 0;
  for (std::uint32_t bs : opts.block_sizes) {
    for (unsigned qd : opts.queue_depths) {
      WorkloadSpec spec = opts.base;
      spec.block_size = bs;
      spec.queue_depth = qd;

      auto run = [&](EngineKind kind) {
        WorkloadResult r;
        Status s = kind == EngineKind::kUring ? RunUring(spec, &r) : RunSync(spec, &r);
        if (!s.ok()) {
          std::fprintf(stderr, "%s bs=%u qd=%u failed: %s\n", EngineName(kind), bs, qd,
                       s.ToString().c_str());
          ++failures;
          return;
        }
        PrintRow(spec, r);
        if (csv.is_open()) AppendCsv(csv, spec, r);
      };
      if (opts.run_uring) run(EngineKind::kUring);
      if (opts.run_sync) run(EngineKind::kSync);
    }
  }
  return failures == 0 ? 0 : 1;
}
