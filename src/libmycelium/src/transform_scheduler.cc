// Copyright (c) 2024-present, Mycelium Authors.
//
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).

#include "mycelium/transform_scheduler.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace mycelium {

// ---------------------------------------------------------------------------
// EWMAAdmissionPolicy — non-inline method implementations
// ---------------------------------------------------------------------------

namespace {

inline uint64_t DoubleToBitsLocal(double d) {
  uint64_t bits;
  static_assert(sizeof(d) == sizeof(bits));
  std::memcpy(&bits, &d, sizeof(bits));
  return bits;
}

inline double BitsToDoubleLocal(uint64_t bits) {
  double d;
  static_assert(sizeof(d) == sizeof(bits));
  std::memcpy(&d, &bits, sizeof(d));
  return d;
}

}  // namespace

void EWMAAdmissionPolicy::UpdateEWMA(double observed) {
  uint64_t old_bits = ewma_bits_.load(std::memory_order_relaxed);
  uint64_t new_bits;
  do {
    double old_val = BitsToDoubleLocal(old_bits);
    double new_val = alpha_ * observed + (1.0 - alpha_) * old_val;
    new_bits = DoubleToBitsLocal(new_val);
  } while (!ewma_bits_.compare_exchange_weak(old_bits, new_bits,
                                              std::memory_order_release,
                                              std::memory_order_relaxed));
}

bool EWMAAdmissionPolicy::ShouldAdmit(double /*cpu_fraction_used*/,
                                       int    /*level*/,
                                       uint64_t /*size*/) const {
  return CurrentEWMA() < cpu_ceiling_;
}

double EWMAAdmissionPolicy::CurrentEWMA() const {
  return BitsToDoubleLocal(ewma_bits_.load(std::memory_order_acquire));
}

// ---------------------------------------------------------------------------
// TransformScheduler
// ---------------------------------------------------------------------------

TransformScheduler::TransformScheduler(const AdmissionPolicy* policy,
                                       CompactionSlackEstimator* estimator,
                                       int compaction_level)
    : policy_(policy),
      estimator_(estimator),
      compaction_level_(compaction_level) {
  assert(policy_    != nullptr);
  assert(estimator_ != nullptr);
}

void TransformScheduler::BeginFile(
    uint64_t file_number,
    const std::vector<std::string>& dest_cf_names,
    uint64_t file_size_bytes) {
  current_file_number_ = file_number;
  current_dest_cfs_    = dest_cf_names;

  double fraction = estimator_->TransformCpuFraction();
  bool admitted   = policy_->ShouldAdmit(fraction, compaction_level_,
                                          file_size_bytes);
  if (admitted) {
    current_decision_ = Decision::kApply;
    files_admitted_++;
  } else {
    current_decision_ = Decision::kDefer;
    files_skipped_++;
    // Pre-populate deferred CFs so even files with zero KVs are captured.
    for (const auto& cf : dest_cf_names) {
      all_deferred_cfs_.push_back(cf);
    }
  }
}

void TransformScheduler::OnFileDone() {
  current_file_number_ = 0;
  current_dest_cfs_.clear();
}

TransformScheduler::Decision TransformScheduler::Decide(
    TransformerType transformer_type, uint64_t /*sst_file_number*/) const {
  if (transformer_type == TransformerType::NOTRANSFORMATION) {
    return Decision::kSkip;
  }
  return current_decision_;
}

void TransformScheduler::OnApplied(uint64_t cpu_ns) {
  estimator_->RecordTransform(cpu_ns, 1);
}

void TransformScheduler::OnDeferred(uint64_t /*sst_file_number*/) {
  for (const auto& cf : current_dest_cfs_) {
    all_deferred_cfs_.push_back(cf);
  }
}

std::vector<std::string> TransformScheduler::DeferredCFs() const {
  std::vector<std::string> result = all_deferred_cfs_;
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

double TransformScheduler::FinalCpuFraction() const {
  return estimator_->TransformCpuFraction();
}

}  // namespace mycelium
