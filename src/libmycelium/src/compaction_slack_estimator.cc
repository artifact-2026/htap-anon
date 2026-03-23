// Copyright (c) 2024-present, Mycelium Authors.
//
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).

#include "mycelium/compaction_slack_estimator.h"

#include <cassert>
#include <cstring>

namespace mycelium {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static int64_t ThreadCpuNs() {
  struct timespec ts;
  // CLOCK_THREAD_CPUTIME_ID gives per-thread CPU time; this is exactly what
  // we want so that background I/O threads don't pollute the measurement.
  int rc = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  if (rc != 0) return 0;
  return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

// ---------------------------------------------------------------------------
// CpuTimer
// ---------------------------------------------------------------------------

CpuTimer::CpuTimer(uint64_t* accumulator) : accumulator_(accumulator) {
  assert(accumulator_ != nullptr);
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &start_);
}

CpuTimer::~CpuTimer() {
  struct timespec end;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &end);
  int64_t elapsed =
      (static_cast<int64_t>(end.tv_sec) - static_cast<int64_t>(start_.tv_sec)) *
          1'000'000'000LL +
      (static_cast<int64_t>(end.tv_nsec) - static_cast<int64_t>(start_.tv_nsec));
  if (elapsed > 0) {
    // Simple non-atomic add: CpuTimer is used only within a single compaction
    // thread (one CompactionSlackEstimator per job), so no data race occurs.
    *accumulator_ += static_cast<uint64_t>(elapsed);
  }
}

// ---------------------------------------------------------------------------
// CompactionSlackEstimator
// ---------------------------------------------------------------------------

void CompactionSlackEstimator::StartCompaction() {
  transform_ns_       = 0;
  transforms_applied_ = 0;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &compaction_start_);
}

void CompactionSlackEstimator::RecordTransform(uint64_t cpu_ns, int count) {
  transform_ns_       += static_cast<int64_t>(cpu_ns);
  transforms_applied_ += count;
}

int64_t CompactionSlackEstimator::CompactionNs() const {
  struct timespec now;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return (static_cast<int64_t>(now.tv_sec) -
          static_cast<int64_t>(compaction_start_.tv_sec)) *
             1'000'000'000LL +
         (static_cast<int64_t>(now.tv_nsec) -
          static_cast<int64_t>(compaction_start_.tv_nsec));
}

double CompactionSlackEstimator::TransformCpuFraction() const {
  int64_t total = CompactionNs();
  if (total <= 0) return 0.0;
  double fraction = static_cast<double>(transform_ns_) /
                    static_cast<double>(total);
  // Clamp to [0, 1] — can slightly exceed 1.0 due to clock resolution.
  if (fraction < 0.0) return 0.0;
  if (fraction > 1.0) return 1.0;
  return fraction;
}

int64_t CompactionSlackEstimator::NowThreadCpuNs() {
  return ThreadCpuNs();
}

}  // namespace mycelium
