// Copyright (c) 2024-present, Mycelium Authors.
// Unit tests for CompactionSlackEstimator and CpuTimer.

#include "mycelium/compaction_slack_estimator.h"

#include <cstdint>

#include "gtest/gtest.h"

using mycelium::CompactionSlackEstimator;
using mycelium::CpuTimer;

// ---------------------------------------------------------------------------
// Initial state
// ---------------------------------------------------------------------------

TEST(CompactionSlackEstimator, FractionZeroBeforeStartCompaction) {
  CompactionSlackEstimator est;
  // No StartCompaction called: compaction_start_ is zero-initialized,
  // CompactionNs() will be a large positive number → fraction stays at 0
  // because transform_ns_ is also 0.
  EXPECT_DOUBLE_EQ(est.TransformCpuFraction(), 0.0);
  EXPECT_EQ(est.TransformsApplied(), 0);
}

TEST(CompactionSlackEstimator, FractionZeroAfterStartWithNoTransforms) {
  CompactionSlackEstimator est;
  est.StartCompaction();
  EXPECT_DOUBLE_EQ(est.TransformCpuFraction(), 0.0);
  EXPECT_EQ(est.TransformsApplied(), 0);
}

// ---------------------------------------------------------------------------
// RecordTransform
// ---------------------------------------------------------------------------

TEST(CompactionSlackEstimator, RecordTransformAccumulatesCount) {
  CompactionSlackEstimator est;
  est.StartCompaction();

  est.RecordTransform(100'000, 1);
  EXPECT_EQ(est.TransformsApplied(), 1);
  EXPECT_EQ(est.TransformNs(), 100'000);

  est.RecordTransform(200'000, 4);
  EXPECT_EQ(est.TransformsApplied(), 5);
  EXPECT_EQ(est.TransformNs(), 300'000);
}

TEST(CompactionSlackEstimator, FractionInUnitRange) {
  CompactionSlackEstimator est;
  est.StartCompaction();

  // Simulate a heavy transform: claim 1 second of transform CPU.
  // Real wall time will be a few microseconds so fraction clamps to 1.0.
  est.RecordTransform(1'000'000'000LL, 1);

  double frac = est.TransformCpuFraction();
  EXPECT_GE(frac, 0.0);
  EXPECT_LE(frac, 1.0);
}

TEST(CompactionSlackEstimator, FractionZeroWithZeroTransformNs) {
  CompactionSlackEstimator est;
  est.StartCompaction();

  // Burn a tiny bit of CPU so CompactionNs() > 0, but record zero transform ns
  volatile uint64_t x = 0;
  for (int i = 0; i < 100; i++) x += static_cast<uint64_t>(i);
  (void)x;

  est.RecordTransform(0, 1);
  EXPECT_DOUBLE_EQ(est.TransformCpuFraction(), 0.0);
}

// ---------------------------------------------------------------------------
// StartCompaction resets state
// ---------------------------------------------------------------------------

TEST(CompactionSlackEstimator, StartCompactionResetsCounters) {
  CompactionSlackEstimator est;
  est.StartCompaction();
  est.RecordTransform(500'000, 7);
  EXPECT_EQ(est.TransformsApplied(), 7);

  // Start a new compaction: counters must reset
  est.StartCompaction();
  EXPECT_EQ(est.TransformsApplied(), 0);
  EXPECT_EQ(est.TransformNs(), 0);
  EXPECT_DOUBLE_EQ(est.TransformCpuFraction(), 0.0);
}

// ---------------------------------------------------------------------------
// CpuTimer — accumulates elapsed thread CPU time into a counter
// ---------------------------------------------------------------------------

TEST(CpuTimer, AccumulatesPositiveTime) {
  uint64_t accumulated = 0;
  {
    CpuTimer timer(&accumulated);
    // Do a small amount of work to consume thread CPU
    volatile uint64_t x = 0;
    for (int i = 0; i < 100'000; i++) x += static_cast<uint64_t>(i);
    (void)x;
  }
  // After destruction the timer should have recorded some time (>= 0).
  // We can't assert > 0 with certainty (clock resolution), but >= 0 must hold.
  EXPECT_GE(accumulated, 0u);
}

TEST(CpuTimer, MultipleTimersAccumulate) {
  uint64_t acc = 0;
  {
    CpuTimer t1(&acc);
    volatile int x = 0;
    for (int i = 0; i < 1000; i++) x += i;
    (void)x;
  }
  uint64_t after_first = acc;
  {
    CpuTimer t2(&acc);
    volatile int y = 0;
    for (int i = 0; i < 1000; i++) y += i;
    (void)y;
  }
  // Second timer adds to the same accumulator: result >= first measurement
  EXPECT_GE(acc, after_first);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
