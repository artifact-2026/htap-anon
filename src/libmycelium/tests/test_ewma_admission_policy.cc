// Copyright (c) 2024-present, Mycelium Authors.
// Unit tests for EWMAAdmissionPolicy.

#include "mycelium/admission_policy.h"

#include "gtest/gtest.h"

using mycelium::AlwaysAdmitPolicy;
using mycelium::EWMAAdmissionPolicy;
using mycelium::NeverAdmitPolicy;

// ---------------------------------------------------------------------------
// AlwaysAdmitPolicy / NeverAdmitPolicy — baseline sanity
// ---------------------------------------------------------------------------

TEST(AlwaysAdmitPolicy, AlwaysReturnsTrue) {
  AlwaysAdmitPolicy policy;
  EXPECT_TRUE(policy.ShouldAdmit(0.0, 0, 0));
  EXPECT_TRUE(policy.ShouldAdmit(1.0, 5, 1 << 20));
  EXPECT_EQ(policy.Name(), "AlwaysAdmitPolicy");
}

TEST(NeverAdmitPolicy, AlwaysReturnsFalse) {
  NeverAdmitPolicy policy;
  EXPECT_FALSE(policy.ShouldAdmit(0.0, 0, 0));
  EXPECT_FALSE(policy.ShouldAdmit(1.0, 5, 1 << 20));
  EXPECT_EQ(policy.Name(), "NeverAdmitPolicy");
}

// ---------------------------------------------------------------------------
// EWMAAdmissionPolicy
// ---------------------------------------------------------------------------

TEST(EWMAAdmissionPolicy, InitialEWMAIsZero) {
  EWMAAdmissionPolicy policy(0.70, 0.2);
  EXPECT_DOUBLE_EQ(policy.CurrentEWMA(), 0.0);
}

TEST(EWMAAdmissionPolicy, ShouldAdmitWhenBelowCeiling) {
  EWMAAdmissionPolicy policy(/*cpu_ceiling=*/0.70, /*alpha=*/0.2);
  // Fresh policy: EWMA = 0 < 0.70 → admit
  EXPECT_TRUE(policy.ShouldAdmit(0.0, 1, 1024));
}

TEST(EWMAAdmissionPolicy, ShouldNotAdmitWhenEWMAExceedsCeiling) {
  EWMAAdmissionPolicy policy(/*cpu_ceiling=*/0.70, /*alpha=*/1.0);
  // alpha=1.0: EWMA = observed immediately
  policy.UpdateEWMA(0.9);
  EXPECT_FALSE(policy.ShouldAdmit(0.0, 1, 1024));
}

TEST(EWMAAdmissionPolicy, ShouldAdmitAtExactCeilingIsFalse) {
  // ShouldAdmit uses strict <, so EWMA == ceiling → reject
  EWMAAdmissionPolicy policy(/*cpu_ceiling=*/0.70, /*alpha=*/1.0);
  policy.UpdateEWMA(0.70);
  EXPECT_FALSE(policy.ShouldAdmit(0.0, 1, 1024));
}

TEST(EWMAAdmissionPolicy, UpdateEWMAWithAlphaOne) {
  // alpha=1.0: new EWMA = observed (immediate, no smoothing)
  EWMAAdmissionPolicy policy(0.70, 1.0);
  policy.UpdateEWMA(0.5);
  EXPECT_DOUBLE_EQ(policy.CurrentEWMA(), 0.5);
  policy.UpdateEWMA(0.8);
  EXPECT_DOUBLE_EQ(policy.CurrentEWMA(), 0.8);
}

TEST(EWMAAdmissionPolicy, UpdateEWMAConvergesWithSmoothingFactor) {
  // alpha=0.5: new_ewma = 0.5 * observed + 0.5 * old_ewma
  EWMAAdmissionPolicy policy(0.70, 0.5);

  // Step 1: 0.5 * 1.0 + 0.5 * 0.0 = 0.5
  policy.UpdateEWMA(1.0);
  EXPECT_NEAR(policy.CurrentEWMA(), 0.5, 1e-9);

  // Step 2: 0.5 * 1.0 + 0.5 * 0.5 = 0.75
  policy.UpdateEWMA(1.0);
  EXPECT_NEAR(policy.CurrentEWMA(), 0.75, 1e-9);

  // Step 3: 0.5 * 1.0 + 0.5 * 0.75 = 0.875
  policy.UpdateEWMA(1.0);
  EXPECT_NEAR(policy.CurrentEWMA(), 0.875, 1e-9);
}

TEST(EWMAAdmissionPolicy, EWMADecaysTowardZero) {
  // Start with a high EWMA, then feed zeros — it should decay.
  EWMAAdmissionPolicy policy(0.70, 0.5);
  policy.UpdateEWMA(1.0);  // EWMA = 0.5
  policy.UpdateEWMA(0.0);  // EWMA = 0.5 * 0 + 0.5 * 0.5 = 0.25
  EXPECT_NEAR(policy.CurrentEWMA(), 0.25, 1e-9);
  policy.UpdateEWMA(0.0);  // 0.5 * 0 + 0.5 * 0.25 = 0.125
  EXPECT_NEAR(policy.CurrentEWMA(), 0.125, 1e-9);
}

TEST(EWMAAdmissionPolicy, OnJobCompleteCallsUpdateEWMA) {
  // OnJobComplete(f) must have the same effect as UpdateEWMA(f).
  EWMAAdmissionPolicy policy_a(0.70, 0.5);
  EWMAAdmissionPolicy policy_b(0.70, 0.5);

  policy_a.UpdateEWMA(0.6);
  policy_b.OnJobComplete(0.6);

  EXPECT_DOUBLE_EQ(policy_a.CurrentEWMA(), policy_b.CurrentEWMA());
}

TEST(EWMAAdmissionPolicy, BaseClassOnJobCompleteIsNoop) {
  // AlwaysAdmitPolicy inherits the default OnJobComplete no-op; calling it
  // must not crash and the policy must still admit afterwards.
  AlwaysAdmitPolicy policy;
  policy.OnJobComplete(0.99);
  EXPECT_TRUE(policy.ShouldAdmit(0.0, 0, 0));
}

TEST(EWMAAdmissionPolicy, Name) {
  EWMAAdmissionPolicy policy;
  EXPECT_EQ(policy.Name(), "EWMAAdmissionPolicy");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
