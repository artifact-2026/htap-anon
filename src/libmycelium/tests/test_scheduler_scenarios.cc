// Copyright (c) 2024-present, Mycelium Authors.
//
// Synthetic scheduler / policy scenario tests (Track B #6).
//
// These tests drive the TransformScheduler and AdmissionPolicy classes through
// realistic multi-file, multi-job sequences without involving a real RocksDB
// database.  Each section corresponds to a claim made in the paper.
//
// Sections:
//   1. ThresholdAdmissionPolicy  — direct ShouldAdmit behaviour
//   2. EWMA feedback loop        — dynamic admission across successive jobs
//   3. R2 W1 correctness gate    — transforms only fire at the bottommost level
//   4. Scheduler aggregate stats — admitted + skipped == total files

#include "mycelium/admission_policy.h"
#include "mycelium/compaction_slack_estimator.h"
#include "mycelium/transform_scheduler.h"
#include "mycelium/transformer.h"

#include "gtest/gtest.h"

using mycelium::AlwaysAdmitPolicy;
using mycelium::CompactionSlackEstimator;
using mycelium::EWMAAdmissionPolicy;
using mycelium::NeverAdmitPolicy;
using mycelium::ThresholdAdmissionPolicy;
using mycelium::TransformScheduler;
using mycelium::TransformerType;
using Decision = TransformScheduler::Decision;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static CompactionSlackEstimator FreshEstimator() {
  CompactionSlackEstimator e;
  e.StartCompaction();
  return e;
}

// Run a single simulated compaction job with `num_files` input SSTs and return
// the counts of admitted and skipped files.
static std::pair<int, int> RunJob(const mycelium::AdmissionPolicy* policy,
                                  int num_files,
                                  int level,
                                  bool is_bottommost) {
  CompactionSlackEstimator est = FreshEstimator();
  TransformScheduler sched(policy, &est, level, is_bottommost);

  int admitted = 0, skipped = 0;
  for (int fn = 1; fn <= num_files; fn++) {
    sched.BeginFile(static_cast<uint64_t>(fn), {"dest_cf"}, 4096);
    auto d = sched.Decide(TransformerType::CONVERTER, static_cast<uint64_t>(fn));
    if (d == Decision::kApply) {
      sched.OnApplied(0);
      admitted++;
    } else {
      sched.OnDeferred(static_cast<uint64_t>(fn));
      skipped++;
    }
    sched.OnFileDone();
  }
  return {sched.FilesAdmitted(), sched.FilesSkipped()};
}

// ============================================================================
// 1. ThresholdAdmissionPolicy
// ============================================================================

TEST(ThresholdAdmissionPolicy, AdmitsBelowCeiling) {
  ThresholdAdmissionPolicy policy(/*cpu_ceiling=*/0.75);
  EXPECT_TRUE(policy.ShouldAdmit(0.0,  0, 0));
  EXPECT_TRUE(policy.ShouldAdmit(0.50, 2, 1024));
  EXPECT_TRUE(policy.ShouldAdmit(0.74, 3, 4096));
}

TEST(ThresholdAdmissionPolicy, RejectsAtCeiling) {
  // ShouldAdmit uses strict < so exactly at ceiling → reject
  ThresholdAdmissionPolicy policy(0.75);
  EXPECT_FALSE(policy.ShouldAdmit(0.75, 1, 1024));
}

TEST(ThresholdAdmissionPolicy, RejectsAboveCeiling) {
  ThresholdAdmissionPolicy policy(0.75);
  EXPECT_FALSE(policy.ShouldAdmit(0.80, 1, 1024));
  EXPECT_FALSE(policy.ShouldAdmit(1.0,  5, 1 << 20));
}

TEST(ThresholdAdmissionPolicy, ZeroCeilingRejectsEverything) {
  ThresholdAdmissionPolicy policy(0.0);
  EXPECT_FALSE(policy.ShouldAdmit(0.0,  0, 0));
  EXPECT_FALSE(policy.ShouldAdmit(0.01, 1, 1024));
}

TEST(ThresholdAdmissionPolicy, OneCeilingAdmitsEverything) {
  ThresholdAdmissionPolicy policy(1.0);
  EXPECT_TRUE(policy.ShouldAdmit(0.0,  0, 0));
  EXPECT_TRUE(policy.ShouldAdmit(0.99, 5, 1 << 20));
}

TEST(ThresholdAdmissionPolicy, OnJobCompleteIsNoop) {
  // ThresholdAdmissionPolicy is stateless: OnJobComplete must not change
  // its admission behaviour.
  ThresholdAdmissionPolicy policy(0.50);
  EXPECT_TRUE(policy.ShouldAdmit(0.4, 1, 1024));
  policy.OnJobComplete(0.99);   // should be a no-op
  EXPECT_TRUE(policy.ShouldAdmit(0.4, 1, 1024));
}

TEST(ThresholdAdmissionPolicy, Name) {
  EXPECT_EQ(ThresholdAdmissionPolicy().Name(), "ThresholdAdmissionPolicy");
}

// ============================================================================
// 2. EWMA feedback loop — dynamic admission across successive jobs
//
// A single EWMAAdmissionPolicy instance persists across compaction jobs.
// After each job, OnJobComplete(cpu_fraction) updates the EWMA.  This section
// verifies that a burst of expensive transforms causes subsequent jobs to defer
// and that admission recovers once CPU usage drops.
// ============================================================================

TEST(EWMAFeedbackLoop, HighCPUJobTriggersBackoff) {
  // alpha=1.0 for instant response; ceiling=0.70
  EWMAAdmissionPolicy policy(/*ceiling=*/0.70, /*alpha=*/1.0);

  // Job 1: EWMA starts at 0 → admits
  {
    auto est = FreshEstimator();
    TransformScheduler sched(&policy, &est, 3, /*bottommost=*/true);
    sched.BeginFile(1, {"dest_cf"}, 4096);
    EXPECT_EQ(sched.Decide(TransformerType::CONVERTER, 1), Decision::kApply)
        << "Job 1: EWMA=0 < ceiling → should admit";
  }

  // Simulate a CPU-heavy job completion: EWMA jumps to 0.9
  policy.OnJobComplete(0.9);
  EXPECT_GT(policy.CurrentEWMA(), 0.70);

  // Job 2: EWMA=0.9 > ceiling → defers
  {
    auto est = FreshEstimator();
    TransformScheduler sched(&policy, &est, 3, /*bottommost=*/true);
    sched.BeginFile(2, {"dest_cf"}, 4096);
    EXPECT_EQ(sched.Decide(TransformerType::CONVERTER, 2), Decision::kDefer)
        << "Job 2: EWMA > ceiling → should defer";
  }
}

TEST(EWMAFeedbackLoop, LowCPUJobRestoresAdmission) {
  EWMAAdmissionPolicy policy(0.70, 1.0);

  // Push EWMA above ceiling
  policy.OnJobComplete(0.9);
  ASSERT_GT(policy.CurrentEWMA(), 0.70);

  // Feed a very low-CPU job: EWMA drops to 0.05 (alpha=1.0 → immediate)
  policy.OnJobComplete(0.05);
  EXPECT_LT(policy.CurrentEWMA(), 0.70);

  // Next job: admits again
  auto est = FreshEstimator();
  TransformScheduler sched(&policy, &est, 3, /*bottommost=*/true);
  sched.BeginFile(3, {"dest_cf"}, 4096);
  EXPECT_EQ(sched.Decide(TransformerType::CONVERTER, 3), Decision::kApply)
      << "After CPU recovery, should admit again";
}

TEST(EWMAFeedbackLoop, ThreeJobConvergenceCycle) {
  // Verify the full high→backoff→recovery cycle using alpha=0.5
  // (more realistic than alpha=1.0 but still predictable).
  //
  // Job 1: EWMA=0 → admit.  After: OnJobComplete(0.9) → EWMA=0.45
  // Job 2: EWMA=0.45 < 0.70 → admit.  After: OnJobComplete(0.9) → EWMA=0.675
  // Job 3: EWMA=0.675 < 0.70 → admit.  After: OnJobComplete(0.9) → EWMA=0.7875
  // Job 4: EWMA=0.7875 > 0.70 → defer.  After: OnJobComplete(0.0) → EWMA=0.39375
  // Job 5: EWMA=0.39375 < 0.70 → admit.
  EWMAAdmissionPolicy policy(/*ceiling=*/0.70, /*alpha=*/0.5);

  auto admit_decision = [&](int file_num) {
    auto est = FreshEstimator();
    TransformScheduler sched(&policy, &est, 3, true);
    sched.BeginFile(static_cast<uint64_t>(file_num), {"dest_cf"}, 4096);
    return sched.Decide(TransformerType::CONVERTER,
                        static_cast<uint64_t>(file_num));
  };

  EXPECT_EQ(admit_decision(1), Decision::kApply);   // EWMA=0
  policy.OnJobComplete(0.9);                         // EWMA=0.45

  EXPECT_EQ(admit_decision(2), Decision::kApply);   // EWMA=0.45 < 0.70
  policy.OnJobComplete(0.9);                         // EWMA=0.675

  EXPECT_EQ(admit_decision(3), Decision::kApply);   // EWMA=0.675 < 0.70
  policy.OnJobComplete(0.9);                         // EWMA=0.7875

  EXPECT_EQ(admit_decision(4), Decision::kDefer);   // EWMA=0.7875 > 0.70
  policy.OnJobComplete(0.0);                         // EWMA=0.39375

  EXPECT_EQ(admit_decision(5), Decision::kApply);   // EWMA=0.39375 < 0.70
}

// ============================================================================
// 3. R2 W1 correctness gate
//
// Claim: Mycelium never applies transformations during non-bottommost
// compactions.  This ensures that transforms only see the canonical
// (fully-merged) value for a key range, never an intermediate version.
//
// In a typical RocksDB LSM:
//   - L0 flush and L0→L1 compactions are usually non-bottommost
//     (data exists at deeper levels).
//   - Only the compaction that outputs to the deepest level for a key range
//     is bottommost.
//
// The test drives a simulated 3-level pipeline and verifies that only the
// final level fires transforms.
// ============================================================================

TEST(CorrectnessGateR2W1, NonBottommostNeverTransforms) {
  // No matter how permissive the policy, non-bottommost → always kDefer.
  AlwaysAdmitPolicy policy;
  const int kNumFiles = 20;

  for (int level = 0; level <= 4; level++) {
    auto [admitted, skipped] = RunJob(&policy, kNumFiles, level,
                                      /*is_bottommost=*/false);
    EXPECT_EQ(admitted, 0)
        << "Level " << level << " non-bottommost: admitted should be 0";
    EXPECT_EQ(skipped, kNumFiles)
        << "Level " << level << " non-bottommost: all files should be skipped";
  }
}

TEST(CorrectnessGateR2W1, OnlyBottommostLevelAppliesTransforms) {
  // Simulate a 4-level compaction pipeline.
  // Levels 0, 1, 2 are non-bottommost; level 3 is bottommost.
  // Only level 3 should apply transforms.
  AlwaysAdmitPolicy policy;
  const int kNumFiles = 5;

  // Non-bottommost levels: all defer
  for (int level = 0; level < 3; level++) {
    auto [admitted, skipped] = RunJob(&policy, kNumFiles, level,
                                      /*is_bottommost=*/false);
    EXPECT_EQ(admitted, 0)   << "Level " << level << ": expected 0 admitted";
    EXPECT_EQ(skipped, kNumFiles) << "Level " << level << ": expected all skipped";
  }

  // Bottommost level: all admitted
  {
    auto [admitted, skipped] = RunJob(&policy, kNumFiles, /*level=*/3,
                                      /*is_bottommost=*/true);
    EXPECT_EQ(admitted, kNumFiles) << "Level 3 bottommost: expected all admitted";
    EXPECT_EQ(skipped, 0)          << "Level 3 bottommost: expected 0 skipped";
  }
}

TEST(CorrectnessGateR2W1, BottommostWithNeverAdmitStillDefers) {
  // Even at the bottommost level, if the policy says "never admit", all
  // files defer.  The gate is bottommost AND policy-admits.
  NeverAdmitPolicy policy;
  auto [admitted, skipped] = RunJob(&policy, 10, /*level=*/3,
                                    /*is_bottommost=*/true);
  EXPECT_EQ(admitted, 0);
  EXPECT_EQ(skipped, 10);
}

// ============================================================================
// 4. Scheduler aggregate stats
//
// The scheduler's FilesAdmitted() + FilesSkipped() must always equal the
// number of files for which BeginFile() was called.
// ============================================================================

TEST(SchedulerStats, AdmittedPlusSkippedEqualsTotalFiles) {
  AlwaysAdmitPolicy policy;
  const int kNumFiles = 17;

  auto est = FreshEstimator();
  TransformScheduler sched(&policy, &est, 3, /*bottommost=*/true);

  for (int fn = 1; fn <= kNumFiles; fn++) {
    sched.BeginFile(static_cast<uint64_t>(fn), {"dest_cf"}, 4096);
    auto d = sched.Decide(TransformerType::CONVERTER,
                           static_cast<uint64_t>(fn));
    if (d == Decision::kApply) {
      sched.OnApplied(0);
    } else {
      sched.OnDeferred(static_cast<uint64_t>(fn));
    }
    sched.OnFileDone();
  }

  EXPECT_EQ(sched.FilesAdmitted() + sched.FilesSkipped(), kNumFiles);
}

TEST(SchedulerStats, AllFilesAdmittedCountedCorrectly) {
  AlwaysAdmitPolicy policy;
  const int kNumFiles = 8;
  auto [admitted, skipped] = RunJob(&policy, kNumFiles, 3, true);
  EXPECT_EQ(admitted, kNumFiles);
  EXPECT_EQ(skipped, 0);
  EXPECT_EQ(admitted + skipped, kNumFiles);
}

TEST(SchedulerStats, AllFilesSkippedWhenNonBottommost) {
  AlwaysAdmitPolicy policy;
  const int kNumFiles = 8;
  auto [admitted, skipped] = RunJob(&policy, kNumFiles, 1, false);
  EXPECT_EQ(admitted, 0);
  EXPECT_EQ(skipped, kNumFiles);
  EXPECT_EQ(admitted + skipped, kNumFiles);
}

TEST(SchedulerStats, NoFilesProcessedYieldsZeroCounts) {
  AlwaysAdmitPolicy policy;
  auto est = FreshEstimator();
  TransformScheduler sched(&policy, &est, 3, true);
  EXPECT_EQ(sched.FilesAdmitted(), 0);
  EXPECT_EQ(sched.FilesSkipped(), 0);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
