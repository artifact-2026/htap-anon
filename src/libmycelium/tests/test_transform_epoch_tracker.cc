// Copyright (c) 2024-present, Mycelium Authors.
// Unit tests for TransformEpochTracker.
//
// TransformEpochTracker is an observability data structure: it records which
// transforms have been applied or skipped for a given SST file, and persists
// that record as table properties.  These tests verify the state-mutation
// methods and the encode/decode round-trip.

#include "mycelium/transform_epoch_tracker.h"

#include <algorithm>
#include <string>
#include <vector>

#include "gtest/gtest.h"

using mycelium::TransformEpochTracker;
using mycelium::TransformState;

// ---------------------------------------------------------------------------
// Initial state
// ---------------------------------------------------------------------------

TEST(TransformEpochTracker, InitialStateIsUnknown) {
  TransformEpochTracker tracker;
  EXPECT_EQ(tracker.GetState("dest_cf"), TransformState::UNKNOWN);
  EXPECT_FALSE(tracker.IsTransformed("dest_cf"));
  EXPECT_FALSE(tracker.IsDeferred("dest_cf"));
  EXPECT_TRUE(tracker.GetDeferredCFs().empty());
  EXPECT_TRUE(tracker.GetAppliedCFs().empty());
}

// ---------------------------------------------------------------------------
// MarkTransformed
// ---------------------------------------------------------------------------

TEST(TransformEpochTracker, MarkTransformedSetsApplied) {
  TransformEpochTracker tracker;
  tracker.MarkTransformed("cf_a");

  EXPECT_EQ(tracker.GetState("cf_a"), TransformState::APPLIED);
  EXPECT_TRUE(tracker.IsTransformed("cf_a"));
  EXPECT_FALSE(tracker.IsDeferred("cf_a"));
}

TEST(TransformEpochTracker, GetAppliedCFsReturnsMarkedCFs) {
  TransformEpochTracker tracker;
  tracker.MarkTransformed("cf_x");
  tracker.MarkTransformed("cf_y");

  auto applied = tracker.GetAppliedCFs();
  std::sort(applied.begin(), applied.end());
  ASSERT_EQ(applied.size(), 2u);
  EXPECT_EQ(applied[0], "cf_x");
  EXPECT_EQ(applied[1], "cf_y");
}

// ---------------------------------------------------------------------------
// MarkDeferred
// ---------------------------------------------------------------------------

TEST(TransformEpochTracker, MarkDeferredSetsDeferred) {
  TransformEpochTracker tracker;
  tracker.MarkDeferred("cf_b");

  EXPECT_EQ(tracker.GetState("cf_b"), TransformState::DEFERRED);
  EXPECT_FALSE(tracker.IsTransformed("cf_b"));
  EXPECT_TRUE(tracker.IsDeferred("cf_b"));
}

TEST(TransformEpochTracker, GetDeferredCFsReturnsMarkedCFs) {
  TransformEpochTracker tracker;
  tracker.MarkDeferred("cf_p");
  tracker.MarkDeferred("cf_q");
  tracker.MarkTransformed("cf_r");  // not deferred

  auto deferred = tracker.GetDeferredCFs();
  std::sort(deferred.begin(), deferred.end());
  ASSERT_EQ(deferred.size(), 2u);
  EXPECT_EQ(deferred[0], "cf_p");
  EXPECT_EQ(deferred[1], "cf_q");
}

// ---------------------------------------------------------------------------
// MarkDeferred does not overwrite an APPLIED entry
// (data-structure invariant: the code explicitly guards this)
// ---------------------------------------------------------------------------

TEST(TransformEpochTracker, MarkDeferredDoesNotOverwriteApplied) {
  TransformEpochTracker tracker;
  tracker.MarkTransformed("cf_a");
  tracker.MarkDeferred("cf_a");  // should be a no-op

  EXPECT_EQ(tracker.GetState("cf_a"), TransformState::APPLIED);
}

TEST(TransformEpochTracker, MarkTransformedOverwritesDeferred) {
  TransformEpochTracker tracker;
  tracker.MarkDeferred("cf_a");
  tracker.MarkTransformed("cf_a");  // catch-up applied

  EXPECT_EQ(tracker.GetState("cf_a"), TransformState::APPLIED);
  EXPECT_FALSE(tracker.IsDeferred("cf_a"));
}

// ---------------------------------------------------------------------------
// ClearDeferred
// ---------------------------------------------------------------------------

TEST(TransformEpochTracker, ClearDeferredRemovesDeferredLeavesApplied) {
  TransformEpochTracker tracker;
  tracker.MarkTransformed("cf_applied");
  tracker.MarkDeferred("cf_deferred");

  tracker.ClearDeferred();

  EXPECT_EQ(tracker.GetState("cf_applied"), TransformState::APPLIED);
  EXPECT_EQ(tracker.GetState("cf_deferred"), TransformState::UNKNOWN);
  EXPECT_TRUE(tracker.GetDeferredCFs().empty());
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

TEST(TransformEpochTracker, ResetClearsAllState) {
  TransformEpochTracker tracker;
  tracker.MarkTransformed("cf_a");
  tracker.MarkDeferred("cf_b");

  tracker.Reset();

  EXPECT_EQ(tracker.GetState("cf_a"), TransformState::UNKNOWN);
  EXPECT_EQ(tracker.GetState("cf_b"), TransformState::UNKNOWN);
  EXPECT_TRUE(tracker.GetDeferredCFs().empty());
  EXPECT_TRUE(tracker.GetAppliedCFs().empty());
}

// ---------------------------------------------------------------------------
// Encode / Decode round-trip
// ---------------------------------------------------------------------------

TEST(TransformEpochTracker, EncodeDecodeEmptyTracker) {
  TransformEpochTracker tracker;
  std::string encoded;
  tracker.EncodeTo(&encoded);

  TransformEpochTracker decoded;
  auto s = decoded.DecodeFrom(encoded);
  EXPECT_TRUE(s.ok()) << s.message();
  EXPECT_EQ(decoded.GetState("any_cf"), TransformState::UNKNOWN);
  EXPECT_TRUE(decoded.GetAppliedCFs().empty());
  EXPECT_TRUE(decoded.GetDeferredCFs().empty());
}

TEST(TransformEpochTracker, EncodeDecodeMixedStates) {
  TransformEpochTracker tracker;
  tracker.MarkTransformed("cf_applied");
  tracker.MarkDeferred("cf_deferred");

  std::string encoded;
  tracker.EncodeTo(&encoded);

  TransformEpochTracker decoded;
  auto s = decoded.DecodeFrom(encoded);
  ASSERT_TRUE(s.ok()) << s.message();

  EXPECT_EQ(decoded.GetState("cf_applied"),  TransformState::APPLIED);
  EXPECT_EQ(decoded.GetState("cf_deferred"), TransformState::DEFERRED);
  EXPECT_EQ(decoded.GetState("cf_unknown"),  TransformState::UNKNOWN);
}

TEST(TransformEpochTracker, EncodeDecodePreservesMultipleCFs) {
  TransformEpochTracker tracker;
  for (int i = 0; i < 10; i++) {
    std::string cf = "cf_" + std::to_string(i);
    if (i % 2 == 0) {
      tracker.MarkTransformed(cf);
    } else {
      tracker.MarkDeferred(cf);
    }
  }

  std::string encoded;
  tracker.EncodeTo(&encoded);

  TransformEpochTracker decoded;
  ASSERT_TRUE(decoded.DecodeFrom(encoded).ok());

  for (int i = 0; i < 10; i++) {
    std::string cf = "cf_" + std::to_string(i);
    if (i % 2 == 0) {
      EXPECT_EQ(decoded.GetState(cf), TransformState::APPLIED) << cf;
    } else {
      EXPECT_EQ(decoded.GetState(cf), TransformState::DEFERRED) << cf;
    }
  }
}

TEST(TransformEpochTracker, DecodeFromCorruptedDataReturnsError) {
  TransformEpochTracker tracker;
  // Pass truncated / garbage bytes
  std::string garbage = "\x05\x00";  // claims 5 records but has no data
  auto s = tracker.DecodeFrom(garbage);
  EXPECT_FALSE(s.ok());
}

TEST(TransformEpochTracker, DecodeFromEmptyStringReturnsError) {
  TransformEpochTracker tracker;
  auto s = tracker.DecodeFrom("");
  EXPECT_FALSE(s.ok());
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
