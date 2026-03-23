// mycelium/compaction_slack_estimator.h — portable CPU estimator (moved in P2)
// Copyright (c) 2024-present, Mycelium Authors.
//
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).
//
// compaction_slack_estimator.h
//
// Measures per-transform CPU overhead inline during a compaction job and
// exposes the fraction of total compaction CPU consumed by transforms.
//
// Usage pattern (inside a single CompactionJob):
//
//   CompactionSlackEstimator estimator;
//   estimator.StartCompaction();
//
//   for each KV pair:
//     {
//       auto guard = estimator.MeasureTransform();
//       // ... call transformer->Transform(...) here ...
//     }  // guard destructor records elapsed CPU time
//
//   double fraction = estimator.TransformCpuFraction();
//   // fraction ∈ [0, 1]: share of total compaction CPU used by transforms

#pragma once

#include <cstdint>
#include <ctime>


namespace mycelium {

// ---------------------------------------------------------------------------
// CpuTimer — RAII helper
// ---------------------------------------------------------------------------
// Records thread CPU time on construction and on destruction.  The elapsed
// nanoseconds are accumulated into a caller-supplied counter.
class CpuTimer {
 public:
  // @param accumulator  Pointer to a nanosecond counter.  Elapsed time is
  //                     added atomically on destruction.
  explicit CpuTimer(uint64_t* accumulator);
  ~CpuTimer();

  // Non-copyable, non-movable.
  CpuTimer(const CpuTimer&) = delete;
  CpuTimer& operator=(const CpuTimer&) = delete;

 private:
  uint64_t* accumulator_;
  struct timespec start_;
};

// ---------------------------------------------------------------------------
// CompactionSlackEstimator
// ---------------------------------------------------------------------------
class CompactionSlackEstimator {
 public:
  CompactionSlackEstimator() = default;

  // Call once at the very beginning of a compaction job (before any KV I/O).
  void StartCompaction();

  // RAII guard that measures CPU consumed by one transform call.
  // Typical use:
  //   uint64_t cpu_ns = 0;
  //   { CpuTimer t(&cpu_ns); transformer->Transform(...); }
  //   RecordTransform(cpu_ns, 1);
  // (CpuTimer is defined in this header.)

  // Record cpu_ns of transform work for `count` KV pairs.
  // Called after each successful transform from TransformScheduler::OnApplied().
  void RecordTransform(uint64_t cpu_ns, int count);

  // No-op: total compaction time is tracked implicitly via StartCompaction().
  // Provided for call-site compatibility.
  void RecordBase(uint64_t /*base_cpu_ns*/, int64_t /*records*/) {}

  // Fraction of total compaction CPU consumed by transforms so far.
  // Returns 0.0 if no compaction time has elapsed yet.
  double TransformCpuFraction() const;

  // Raw nanosecond counters — exposed for logging.
  int64_t TransformNs()    const { return transform_ns_; }
  int64_t CompactionNs()   const;
  int64_t TransformsApplied() const { return transforms_applied_; }

 private:
  struct timespec compaction_start_{};
  int64_t transform_ns_      = 0;
  int64_t transforms_applied_ = 0;

  static int64_t NowThreadCpuNs();
};

}  // namespace mycelium
