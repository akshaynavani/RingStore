#include <gtest/gtest.h>

#include <cmath>

#include "histogram.h"

namespace ringstore {
namespace bench {
namespace {

TEST(LatencyHistogramTest, EmptyHistogram) {
  LatencyHistogram h;
  EXPECT_EQ(h.count(), 0u);
  EXPECT_EQ(h.min(), 0u);
  EXPECT_EQ(h.Percentile(0.99), 0u);
  EXPECT_EQ(h.mean(), 0.0);
}

TEST(LatencyHistogramTest, BucketBoundsContainValue) {
  for (std::uint64_t v : {0ull, 1ull, 31ull, 32ull, 33ull, 63ull, 64ull, 1000ull, 123456789ull,
                          (1ull << 40) + 12345, ~0ull}) {
    std::size_t idx = LatencyHistogram::Index(v);
    EXPECT_GE(LatencyHistogram::BucketUpper(idx), v) << v;
    if (idx > 0) EXPECT_LT(LatencyHistogram::BucketUpper(idx - 1), v) << v;
  }
}

TEST(LatencyHistogramTest, IndexIsMonotonic) {
  std::size_t prev = 0;
  for (std::uint64_t v = 0; v < 1'000'000; v += 7) {
    std::size_t idx = LatencyHistogram::Index(v);
    ASSERT_GE(idx, prev);
    prev = idx;
  }
}

TEST(LatencyHistogramTest, PercentilesOfUniformDistribution) {
  LatencyHistogram h;
  for (std::uint64_t v = 1; v <= 100000; ++v) h.Record(v);
  EXPECT_EQ(h.count(), 100000u);
  EXPECT_EQ(h.min(), 1u);
  EXPECT_EQ(h.max(), 100000u);
  EXPECT_NEAR(h.mean(), 50000.5, 1e-6);
  auto within = [](std::uint64_t got, double want) {
    return std::fabs(static_cast<double>(got) - want) / want < 0.04;
  };
  EXPECT_TRUE(within(h.Percentile(0.50), 50000)) << h.Percentile(0.50);
  EXPECT_TRUE(within(h.Percentile(0.99), 99000)) << h.Percentile(0.99);
  EXPECT_EQ(h.Percentile(1.0), 100000u);
}

TEST(LatencyHistogramTest, MergeCombinesCounts) {
  LatencyHistogram a;
  LatencyHistogram b;
  for (int i = 0; i < 100; ++i) a.Record(10);
  for (int i = 0; i < 100; ++i) b.Record(1'000'000);
  a.Merge(b);
  EXPECT_EQ(a.count(), 200u);
  EXPECT_EQ(a.min(), 10u);
  EXPECT_EQ(a.max(), 1'000'000u);
  EXPECT_EQ(a.Percentile(0.25), 10u);
  EXPECT_GE(a.Percentile(0.75), 1'000'000u * 96 / 100);
}

}  // namespace
}  // namespace bench
}  // namespace ringstore
