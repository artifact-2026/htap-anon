// mycelium/admission_policy.h — portable admission control (moved in P2)
// Copyright (c) 2024-present, Mycelium Authors.
//
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).
//
// admission_policy.h
//
// Public interface for Mycelium admission control. An AdmissionPolicy
// answers one question per SST file being compacted:
//
//   "Given this compaction's current CPU slack budget, should we apply
//    user transformations to the key-value pairs in this file?"
//
// The policy is consulted once per SST file, before the first KV pair
// from that file is processed, via TransformScheduler::Decide().
// Returning false causes the compaction job to skip transformation for
// the entire file and record it for deferred catch-up compaction.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace mycelium {

// ---------------------------------------------------------------------------
// AdmissionPolicy — base class
// ---------------------------------------------------------------------------
//
// Thread-safety: all virtual methods must be safe to call from multiple
// compaction threads concurrently. Implementations should use atomics or
// internal locks.
class AdmissionPolicy {
public:
  virtual ~AdmissionPolicy() = default;

  // Called once before each compaction file's first KV pair.
  //
  // @param cpu_fraction_used  Estimated fraction of allotted CPU budget already
  //                           consumed by transforms in this compaction job
  //                           (0.0 = none used, 1.0 = budget exhausted).
  // @param compaction_level   Output level for the compaction (0 = L0→L1, etc.)
  // @param file_size_bytes    Uncompressed byte count of the SST being
  // examined.
  //
  // @return true  → apply transforms to this file.
  //         false → skip transforms; file will be enqueued for deferred
  //         compaction.
  virtual bool ShouldAdmit(double cpu_fraction_used, int compaction_level,
                           uint64_t file_size_bytes) const = 0;

  // Called once after every completed compaction job with the final observed
  // CPU fraction for that job.  The default is a no-op; stateful policies
  // (e.g. EWMAAdmissionPolicy) override this to update their internal state.
  // Must be safe to call concurrently from multiple compaction threads.
  virtual void OnJobComplete(double /*cpu_fraction*/) const {}

  // Human-readable name for logging / metrics.
  virtual std::string Name() const = 0;
};

// ---------------------------------------------------------------------------
// AlwaysAdmitPolicy — baseline / testing
// ---------------------------------------------------------------------------
// Matches the behaviour of the original Mycelium: every file is transformed.
class AlwaysAdmitPolicy : public AdmissionPolicy {
public:
  bool ShouldAdmit(double /*cpu_fraction_used*/, int /*compaction_level*/,
                   uint64_t /*file_size_bytes*/) const override {
    return true;
  }
  std::string Name() const override { return "AlwaysAdmitPolicy"; }
};

// ---------------------------------------------------------------------------
// NeverAdmitPolicy — testing / dry-run mode
// ---------------------------------------------------------------------------
class NeverAdmitPolicy : public AdmissionPolicy {
public:
  bool ShouldAdmit(double /*cpu_fraction_used*/, int /*compaction_level*/,
                   uint64_t /*file_size_bytes*/) const override {
    return false;
  }
  std::string Name() const override { return "NeverAdmitPolicy"; }
};

// ---------------------------------------------------------------------------
// ThresholdAdmissionPolicy
// ---------------------------------------------------------------------------
// Admits transforms while the consumed-CPU fraction is below a fixed ceiling.
// Once the ceiling is breached for this compaction job, all remaining files
// are deferred.
//
// Typical setting:  cpu_ceiling = 0.75  (allow up to 75% of compaction CPU)
class ThresholdAdmissionPolicy : public AdmissionPolicy {
public:
  // @param cpu_ceiling  Fraction in [0, 1]. Files are admitted while
  //                     cpu_fraction_used < cpu_ceiling.
  explicit ThresholdAdmissionPolicy(double cpu_ceiling = 0.75)
      : cpu_ceiling_(cpu_ceiling) {}

  bool ShouldAdmit(double cpu_fraction_used, int /*compaction_level*/,
                   uint64_t /*file_size_bytes*/) const override {
    return cpu_fraction_used < cpu_ceiling_;
  }

  std::string Name() const override { return "ThresholdAdmissionPolicy"; }

  double cpu_ceiling() const { return cpu_ceiling_; }

private:
  const double cpu_ceiling_;
};

// ---------------------------------------------------------------------------
// EWMAAdmissionPolicy
// ---------------------------------------------------------------------------
// Tracks an Exponentially Weighted Moving Average of observed CPU fractions
// across all recently completed compaction jobs. Admits if the EWMA is below
// a configurable ceiling.
//
// alpha controls the smoothing factor (closer to 1 = more reactive).
// Typical setting: alpha = 0.2, cpu_ceiling = 0.70
class EWMAAdmissionPolicy : public AdmissionPolicy {
public:
  explicit EWMAAdmissionPolicy(double cpu_ceiling = 0.70, double alpha = 0.2)
      : cpu_ceiling_(cpu_ceiling), alpha_(alpha) {}

  // Update the EWMA with the CPU fraction observed in one completed job.
  // const because ewma_bits_ is mutable; safe to call from multiple threads.
  void UpdateEWMA(double observed_fraction) const;

  bool ShouldAdmit(double /*cpu_fraction_used*/, int /*compaction_level*/,
                   uint64_t /*file_size_bytes*/) const override;

  void OnJobComplete(double cpu_fraction) const override {
    UpdateEWMA(cpu_fraction);
  }

  double CurrentEWMA() const;

  std::string Name() const override { return "EWMAAdmissionPolicy"; }

private:
  const double cpu_ceiling_;
  const double alpha_;
  // Stored as integer bits so we can do a lock-free compare-exchange update
  // from any compaction thread without holding a mutex.
  mutable std::atomic<uint64_t> ewma_bits_{0};
};

} // namespace mycelium
