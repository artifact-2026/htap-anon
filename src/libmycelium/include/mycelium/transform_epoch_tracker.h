// mycelium/transform_epoch_tracker.h — portable epoch tracker (moved in P2)
// Copyright (c) 2024-present, Mycelium Authors.
//
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).
//
// transform_epoch_tracker.h
//
// Per-SSTable record of which user transformations have been applied and which
// are deferred.  This prevents the system from re-applying a non-reentrant
// transform to an SST file that was already transformed in a previous
// compaction.
//
// Persistence:
//   The tracker serialises its state to a byte string that is stored in the
//   SST's custom properties block (via TablePropertiesCollector) or in a
//   side-channel manifest entry.  The exact persistence mechanism is left to
//   the integrator; the codec below handles the byte layout.
//
// Layout (little-endian):
//   [4 bytes] uint32 number of CF records
//   per record:
//     [2 bytes] uint16 CF name length
//     [N bytes] CF name (UTF-8, not null-terminated)
//     [1 byte]  uint8  TransformState (APPLIED=1, DEFERRED=2)
//
// Thread-safety: NOT thread-safe.  One TransformEpochTracker is created per
// SST file and is accessed only from the compaction thread that owns it.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <string_view>

// mycelium::Status replaces rocksdb::Status — no RocksDB header needed here.
#include "mycelium/status.h"

namespace mycelium {

// ---------------------------------------------------------------------------
// TransformState
// ---------------------------------------------------------------------------
enum class TransformState : uint8_t {
  UNKNOWN  = 0,
  APPLIED  = 1,  // transform was executed and output written to dest CF
  DEFERRED = 2,  // transform was skipped; catch-up compaction is needed
};

// ---------------------------------------------------------------------------
// TransformEpochTracker
// ---------------------------------------------------------------------------
class TransformEpochTracker {
 public:
  TransformEpochTracker() = default;

  // ------- Query -------

  // Returns true iff the transform targeting destination CF `cf_name` has
  // already been applied to this SST.  Prevents double-application of
  // non-reentrant transforms.
  bool IsTransformed(const std::string& cf_name) const;

  // Returns true iff the transform was explicitly deferred (skipped) for
  // this SST during a prior compaction.
  bool IsDeferred(const std::string& cf_name) const;

  TransformState GetState(const std::string& cf_name) const;

  // Returns all CF names that are currently in DEFERRED state.
  std::vector<std::string> GetDeferredCFs() const;

  // Returns all CF names that are currently in APPLIED state.
  std::vector<std::string> GetAppliedCFs() const;

  // ------- Update -------

  // Records that the transform for destination CF `cf_name` completed
  // successfully.  Overwrites any previous DEFERRED entry.
  void MarkTransformed(const std::string& cf_name);

  // Records that the transform for destination CF `cf_name` was skipped for
  // this compaction run.  Does NOT overwrite an existing APPLIED entry (i.e.,
  // a file that was already transformed cannot be re-deferred).
  void MarkDeferred(const std::string& cf_name);

  // Clears all DEFERRED entries.  Call after the deferred CFs have been
  // scheduled for catch-up compaction.
  void ClearDeferred();

  // Clears all state (for testing / reuse).
  void Reset();

  // ------- Serialisation -------

  // Appends the encoded tracker state to `*out`.
  void EncodeTo(std::string* out) const;

  // Decodes state from the byte span starting at `input.data()`.
  // Returns an error if the encoding is malformed.
  mycelium::Status DecodeFrom(std::string_view input);

 private:
  // cf_name → state
  std::unordered_map<std::string, TransformState> states_;
};

}  // namespace mycelium
