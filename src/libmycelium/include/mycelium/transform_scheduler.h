// mycelium/transform_scheduler.h — portable scheduler (moved in P2)
// Copyright (c) 2024-present, Mycelium Authors.
//
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).
//
// transform_scheduler.h
//
// TransformScheduler decides, per KV pair, whether the user transform should
// be applied or deferred, based on the AdmissionPolicy and current CPU budget.
//
// Lifecycle within a single CompactionJob:
//
//   TransformScheduler sched(policy, &estimator, output_level);
//
//   // For each input SST file:
//   sched.BeginFile(file_number, dest_cf_names, file_size_bytes);
//
//   // For each KV pair in that file (called from AddToOutput):
//   auto decision = sched.Decide(transformer_type, sst_file_number);
//   if (decision == TransformScheduler::Decision::kApply) {
//     uint64_t cpu_ns = 0;
//     { CpuTimer t(&cpu_ns); transformer->Transform(...); }
//     sched.OnApplied(cpu_ns);
//   } else {
//     sched.OnDeferred(sst_file_number);
//   }
//
//   sched.OnFileDone();
//
//   // After all files:
//   std::vector<std::string> deferred = sched.DeferredCFs();

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mycelium/compaction_slack_estimator.h"
#include "mycelium/admission_policy.h"
#include "mycelium/transformer.h"   // for TransformerType

namespace mycelium {

// ---------------------------------------------------------------------------
// TransformScheduler
// ---------------------------------------------------------------------------
class TransformScheduler {
 public:
  // Per-KV admission decision.
  enum class Decision {
    kApply,   // Run the transform; write output to destination CF(s).
    kDefer,   // Skip transform; passthrough write; record file as deferred.
    kSkip,    // No transform configured (NOTRANSFORMATION); plain passthrough.
  };

  // @param policy           Admission policy. Borrowed; must outlive this.
  // @param estimator        CPU estimator for this job. Borrowed; must outlive this.
  // @param compaction_level Output level of the compaction (0 = L0→L1, etc.)
  TransformScheduler(const AdmissionPolicy* policy,
                     CompactionSlackEstimator* estimator,
                     int compaction_level);

  // ------- Per-file lifecycle (called from CompactionJob) -------

  // Call once before the first KV of a new input SST.
  // @param file_number    RocksDB FileNumber of this input SST.
  // @param dest_cf_names  Destination CF names for all transforms.
  // @param file_size_bytes  Uncompressed size (used by policy).
  void BeginFile(uint64_t file_number,
                 const std::vector<std::string>& dest_cf_names,
                 uint64_t file_size_bytes = 0);

  // Call once after the last KV of the current file.
  void OnFileDone();

  // ------- Per-KV decision (called from CompactionOutputs::AddToOutput) -------

  // Returns the admission decision for the given KV pair.
  // @param transformer_type  The transformer type on this CF.
  // @param sst_file_number   File number of the SST containing this KV.
  Decision Decide(TransformerType transformer_type,
                  uint64_t sst_file_number) const;

  // ------- Per-KV callbacks -------

  // Call after successfully applying the transform for a KV pair.
  // @param cpu_ns  Thread CPU nanoseconds consumed by the transform call.
  void OnApplied(uint64_t cpu_ns);

  // Call when a KV pair's transform was deferred (file-level skip).
  // @param sst_file_number  File containing the deferred KV.
  void OnDeferred(uint64_t sst_file_number);

  // ------- End-of-job summary -------

  // Unique destination CF names that were deferred across all files.
  std::vector<std::string> DeferredCFs() const;

  // Final CPU fraction as measured by the estimator.
  double FinalCpuFraction() const;

  // Admission stats for logging.
  int FilesAdmitted() const { return files_admitted_; }
  int FilesSkipped()  const { return files_skipped_;  }

 private:
  const AdmissionPolicy*    policy_;
  CompactionSlackEstimator* estimator_;
  const int                 compaction_level_;

  // Decision set in BeginFile(), returned by Decide() for all KVs in the file.
  Decision                  current_decision_ = Decision::kSkip;
  uint64_t                  current_file_number_ = 0;

  // Destination CF names for the current file.
  std::vector<std::string>  current_dest_cfs_;

  // Accumulated deferred CF names across all files (may have duplicates;
  // de-dup happens in DeferredCFs()).
  std::vector<std::string>  all_deferred_cfs_;

  int files_admitted_ = 0;
  int files_skipped_  = 0;
};

}  // namespace mycelium
