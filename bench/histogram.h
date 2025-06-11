#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace ringstore {
namespace bench {

// Fixed-memory latency histogram in the spirit of HdrHistogram: values are
// bucketed by power of two (major bucket) and then linearly into
// kSubBuckets sub-buckets, giving a worst-case relative error of
// 1/kSubBuckets (~3%) across the full 64-bit range.
class LatencyHistogram {
 public:
  static constexpr int kSubBits = 5;
  static constexpr int kSubBuckets = 1 << kSubBits;
  static constexpr int kMajor = 64;

  void Record(std::uint64_t value) {
    ++counts_[Index(value)];
    ++total_;
    sum_ += value;
    min_ = std::min(min_, value);
    max_ = std::max(max_, value);
  }

  void Merge(const LatencyHistogram& other) {
    for (std::size_t i = 0; i < counts_.size(); ++i) counts_[i] += other.counts_[i];
    total_ += other.total_;
    sum_ += other.sum_;
    min_ = std::min(min_, other.min_);
    max_ = std::max(max_, other.max_);
  }

  void Reset() { *this = LatencyHistogram(); }

  std::uint64_t count() const { return total_; }
  std::uint64_t min() const { return total_ == 0 ? 0 : min_; }
  std::uint64_t max() const { return max_; }
  double mean() const { return total_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(total_); }

  // Returns an upper bound of the bucket containing the q-th quantile
  // (q in [0, 1]), clamped to the observed max.
  std::uint64_t Percentile(double q) const {
    if (total_ == 0) return 0;
    q = std::clamp(q, 0.0, 1.0);
    auto rank = static_cast<std::uint64_t>(q * static_cast<double>(total_));
    if (rank == 0) rank = 1;
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < counts_.size(); ++i) {
      seen += counts_[i];
      if (seen >= rank) return std::min(BucketUpper(i), max_);
    }
    return max_;
  }

  // Exposed for testing bucket math.
  static std::size_t Index(std::uint64_t v) {
    if (v < static_cast<std::uint64_t>(kSubBuckets)) return static_cast<std::size_t>(v);
    int msb = 63 - __builtin_clzll(v);
    int shift = msb - kSubBits;
    auto sub = static_cast<std::size_t>((v >> shift) & (kSubBuckets - 1));
    return static_cast<std::size_t>(shift + 1) * kSubBuckets + sub;
  }

  static std::uint64_t BucketUpper(std::size_t index) {
    if (index < static_cast<std::size_t>(kSubBuckets)) return index;
    std::size_t shift = index / kSubBuckets - 1;
    std::uint64_t sub = index % kSubBuckets;
    std::uint64_t base = (static_cast<std::uint64_t>(kSubBuckets) | sub) << shift;
    return base + ((std::uint64_t{1} << shift) - 1);
  }

 private:
  std::array<std::uint64_t, (kMajor - kSubBits + 1) * kSubBuckets> counts_{};
  std::uint64_t total_ = 0;
  std::uint64_t sum_ = 0;
  std::uint64_t min_ = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t max_ = 0;
};

}  // namespace bench
}  // namespace ringstore
