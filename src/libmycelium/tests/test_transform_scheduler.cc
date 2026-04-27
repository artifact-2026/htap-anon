// Copyright (c) 2024-present, Mycelium Authors.
// Unit tests for TransformScheduler.

#include "mycelium/transform_scheduler.h"

#include <string>
#include <vector>

#include "mycelium/admission_policy.h"
#include "mycelium/compaction_slack_estimator.h"
#include "mycelium/transformer.h"   // TransformerType

#include "gtest/gtest.h"

using mycelium::AlwaysAdmitPolicy;
using mycelium::CompactionSlackEstimator;
using mycelium::NeverAdmitPolicy;
using mycelium::TransformScheduler;
using mycelium::TransformerType;
using Decision = TransformScheduler::Decision;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a ready-to-use estimator (StartCompaction resets internal clock).
static CompactionSlackEstimator MakeEstimator() {
  CompactionSlackEstimator e;
  e.StartCompaction();
  return e;
}

// ---------------------------------------------------------------------------
// NOTRANSFORMATION — always kSkip regardless of level / policy
// ---------------------------------------------------------------------------

TEST(TransformScheduler, NoTransformationSkipsAtBottommost) {
  AlwaysAdmitPolicy policy;
  auto est = MakeEstimator();
  TransformScheduler sched(&policy, &est, /*level=*/3, /*is_bottommost=*/true);

  sched.BeginFile(1, {"dest_cf"}, 1024);
  EXPECT_EQ(sched.Decide(TransformerType::NOTRANSFORMATION, 1), Decision::kSkip);
}

TEST(TransformScheduler, NoTransformationSkipsAtNonBottommost) {
  AlwaysAdmitPolicy policy;
  auto est = MakeEstimator();
  TransformScheduler sched(&policy, &est, /*level=*/1, /*is_bottommost=*/false);

  sched.BeginFile(1, {"dest_cf"}, 1024);
  EXPECT_EQ(sched.Decide(TransformerType::NOTRANSFORMATION, 1), Decision::kSkip);
}

// ---------------------------------------------------------------------------
// Non-bottommost: always kDefer, regardless of admission policy
// ---------------------------------------------------------------------------

TEST(TransformScheduler, NonBottommostAlwaysDeferWithAlwaysAdmit) {
  AlwaysAdmitPolicy policy;
  auto est = MakeEstimator();
  TransformScheduler sched(&policy, &est, /*level=*/1, /*is_bottommost=*/false);

  sched.BeginFile(1, {"dest_cf"}, 1024);
  EXPECT_EQ(sched.Decide(TransformerType::CONVERTER, 1), Decision::kDefer);
  EXPECT_EQ(sched.FilesSkipped(), 1);
  EXPECT_EQ(sched.FilesAdmitted(), 0);
}

TEST(TransformScheduler, NonBottommostAlwaysDeferWithNeverAdmit) {
  NeverAdmitPolicy policy;
  auto est = MakeEstimator();
  TransformScheduler sched(&policy, &est, /*level=*/2, /*is_bottommost=*/false);

  sched.BeginFile(5, {"dest_cf"}, 2048);
  EXPECT_EQ(sched.Decide(TransformerType::DISTRIBUTOR, 5), Decision::kDefer);
}

TEST(TransformScheduler, NonBottommostMultipleFilesAllDeferred) {
  AlwaysAdmitPolicy policy;
  auto est = MakeEstimator();
  TransformScheduler sched(&policy, &est, /*level=*/0, /*is_bottommost=*/false);

  for (uint64_t fn = 1; fn <= 5; fn++) {
    sched.BeginFile(fn, {"dest_cf"}, 1024);
    EXPECT_EQ(sched.Decide(TransformerType::AUGMENTER, fn), Decision::kDefer);
    sched.OnFileDone();
  }
  EXPECT_EQ(sched.FilesSkipped(), 5);
  EXPECT_EQ(sched.FilesAdmitted(), 0);
}

// ---------------------------------------------------------------------------
// Bottommost + AlwaysAdmit → kApply
// ---------------------------------------------------------------------------

TEST(TransformScheduler, BottommostAlwaysAdmitApplies) {
  AlwaysAdmitPolicy policy;
  auto est = MakeEstimator();
  TransformScheduler sched(&policy, &est, /*level=*/3, /*is_bottommost=*/true);

  sched.BeginFile(10, {"dest_cf"}, 1024);
  EXPECT_EQ(sched.Decide(TransformerType::CONVERTER, 10), Decision::kApply);
  EXPECT_EQ(sched.FilesAdmitted(), 1);
  EXPECT_EQ(sched.FilesSkipped(), 0);
}

// ---------------------------------------------------------------------------
// nullptr policy — zero-overhead always-admit fast-path
// ---------------------------------------------------------------------------

TEST(TransformScheduler, NullPolicyBottommostAlwaysApplies) {
  // nullptr admission_policy: transformers are configured but no policy is set.
  // Must behave identically to AlwaysAdmitPolicy with no virtual dispatch.
  // (Canonical RocksDB users with no transformers never construct a
  // TransformScheduler at all — they never reach this code.)
  auto est = MakeEstimator();
  TransformScheduler sched(/*policy=*/nullptr, &est, /*level=*/3,
                            /*is_bottommost=*/true);

  sched.BeginFile(1, {"dest_cf"}, 1024);
  EXPECT_EQ(sched.Decide(TransformerType::CONVERTER, 1), Decision::kApply);
  EXPECT_EQ(sched.FilesAdmitted(), 1);
  EXPECT_EQ(sched.FilesSkipped(), 0);
}

TEST(TransformScheduler, NullPolicyNonBottommostStillDefers) {
  // The bottommost gate must fire before the nullptr fast-path: even with
  // nullptr policy a non-bottommost compaction must defer.
  auto est = MakeEstimator();
  TransformScheduler sched(nullptr, &est, /*level=*/1, /*is_bottommost=*/false);

  sched.BeginFile(1, {"dest_cf"}, 1024);
  EXPECT_EQ(sched.Decide(TransformerType::CONVERTER, 1), Decision::kDefer);
  EXPECT_EQ(sched.FilesAdmitted(), 0);
  EXPECT_EQ(sched.FilesSkipped(), 1);
}

// ---------------------------------------------------------------------------
// Bottommost + NeverAdmit → kDefer
// ---------------------------------------------------------------------------

TEST(TransformScheduler, BottommostNeverAdmitDefers) {
  NeverAdmitPolicy policy;
  auto est = MakeEstimator();
  TransformScheduler sched(&policy, &est, /*level=*/3, /*is_bottommost=*/true);

  sched.BeginFile(10, {"dest_cf"}, 1024);
  EXPECT_EQ(sched.Decide(TransformerType::CONVERTER, 10), Decision::kDefer);
  EXPECT_EQ(sched.FilesSkipped(), 1);
}

// ---------------------------------------------------------------------------
// Decision is file-level (all KVs in a file share the same decision)
// ---------------------------------------------------------------------------

TEST(TransformScheduler, AllKVsInFileSameDecision) {
  AlwaysAdmitPolicy policy;
  auto est = MakeEstimator();
  TransformScheduler sched(&policy, &est, 3, true);

  sched.BeginFile(1, {"dest_cf"}, 1024);
  // Call Decide many times for the same file — all must return kApply
  for (int i = 0; i < 100; i++) {
    EXPECT_EQ(sched.Decide(TransformerType::DISTRIBUTOR, 1), Decision::kApply);
  }
}

// ---------------------------------------------------------------------------
// DeferredCFs — deduplicated across files
// ---------------------------------------------------------------------------

TEST(TransformScheduler, DeferredCFsDeduplicated) {
  NeverAdmitPolicy policy;
  auto est = MakeEstimator();
  TransformScheduler sched(&policy, &est, 3, true);

  // Three files, all deferring to the same two dest CFs
  for (uint64_t fn = 1; fn <= 3; fn++) {
    sched.BeginFile(fn, {"cf_a", "cf_b"}, 1024);
    sched.OnDeferred(fn);
    sched.OnFileDone();
  }

  auto deferred = sched.DeferredCFs();
  ASSERT_EQ(deferred.size(), 2u);
  // DeferredCFs() sorts before dedup, so order is lexicographic
  EXPECT_EQ(deferred[0], "cf_a");
  EXPECT_EQ(deferred[1], "cf_b");
}

TEST(TransformScheduler, DeferredCFsEmptyWhenAllAdmitted) {
  AlwaysAdmitPolicy policy;
  auto est = MakeEstimator();
  TransformScheduler sched(&policy, &est, 3, true);

  sched.BeginFile(1, {"dest_cf"}, 1024);
  // Admitted — OnApplied, not OnDeferred
  sched.OnApplied(0);
  sched.OnFileDone();

  EXPECT_TRUE(sched.DeferredCFs().empty());
}

// ---------------------------------------------------------------------------
// DeferredFileNumbers — deduplicated
// ---------------------------------------------------------------------------

TEST(TransformScheduler, DeferredFileNumbersDeduplicated) {
  NeverAdmitPolicy policy;
  auto est = MakeEstimator();
  TransformScheduler sched(&policy, &est, 3, true);

  // Same file number called multiple times in OnDeferred
  sched.BeginFile(42, {"dest_cf"}, 1024);
  sched.OnDeferred(42);
  sched.OnDeferred(42);
  sched.OnDeferred(42);
  sched.OnFileDone();

  auto file_nums = sched.DeferredFileNumbers();
  ASSERT_EQ(file_nums.size(), 1u);
  EXPECT_EQ(file_nums[0], 42u);
}

TEST(TransformScheduler, DeferredFileNumbersMultipleFiles) {
  NeverAdmitPolicy policy;
  auto est = MakeEstimator();
  TransformScheduler sched(&policy, &est, 3, true);

  for (uint64_t fn : {10u, 20u, 30u}) {
    sched.BeginFile(fn, {"dest_cf"}, 1024);
    sched.OnDeferred(fn);
    sched.OnFileDone();
  }

  auto file_nums = sched.DeferredFileNumbers();
  ASSERT_EQ(file_nums.size(), 3u);
  EXPECT_EQ(file_nums[0], 10u);
  EXPECT_EQ(file_nums[1], 20u);
  EXPECT_EQ(file_nums[2], 30u);
}

// ---------------------------------------------------------------------------
// OnApplied feeds the estimator
// ---------------------------------------------------------------------------

TEST(TransformScheduler, OnAppliedUpdatesFinalFraction) {
  AlwaysAdmitPolicy policy;
  auto est = MakeEstimator();
  TransformScheduler sched(&policy, &est, 3, true);

  EXPECT_DOUBLE_EQ(sched.FinalCpuFraction(), 0.0);

  sched.BeginFile(1, {"dest_cf"}, 1024);
  sched.OnApplied(1'000'000);  // 1 ms of claimed transform CPU
  sched.OnFileDone();

  // Fraction >= 0 after recording some transform time
  EXPECT_GE(sched.FinalCpuFraction(), 0.0);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
