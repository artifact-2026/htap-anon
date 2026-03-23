#pragma once

// mycelium/compaction_hook.h
//
// Four pure-virtual interfaces that form the contract between libmycelium and
// any host LSM engine.  The RocksDB adapter implements all four.  A future
// LevelDB or WiredTiger adapter would provide its own implementations.
//
// Interface summary:
//   CompactionWriter  — emit transformed KV pairs into destination trees
//   GroveManager      — propagate deletes / tombstones across the grove
//   DeferCallback     — reschedule transforms deferred by the admission layer
//   EpochStore        — persist per-SST transform epoch metadata
//
// All methods return mycelium::Status so the portable core never touches
// rocksdb::Status.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mycelium/status.h"

// Forward-declare to avoid pulling in Arrow headers in this interface file.
// Callers that need the full Arrow types #include <arrow/api.h> directly.
namespace arrow { class RecordBatch; }

namespace mycelium {

// Forward declarations within mycelium
class TransformEpochTracker;

// ── CompactionWriter ──────────────────────────────────────────────────────────
//
// Emits one transformed KV pair into one destination tree.
//
// dest_index  — 0-based index into GroveNode::dest_names.
// key         — the (possibly modified) key for this output record.
// value       — the serialised output value bytes.
//
// The engine adapter is responsible for routing the write to the correct
// column family / table and for handling write options / WAL behaviour.
//
class CompactionWriter {
 public:
  virtual ~CompactionWriter() = default;

  // Write one KV pair to destination tree [dest_index].
  virtual Status EmitKV(size_t          dest_index,
                        std::string_view key,
                        std::string_view value) = 0;

  // Flush any buffered writes.  Called once per compaction output file.
  virtual Status Flush() { return Status::OK(); }

  // Number of destination trees registered with this writer.
  virtual size_t DestCount() const = 0;
};

// ── GroveManager ──────────────────────────────────────────────────────────────
//
// Manages grove-level operations that span multiple trees: delete propagation
// and any future grove maintenance (e.g., merging derived trees).
//
// The RocksDB adapter implements this by iterating over all derived CFs and
// issuing Delete() writes.
//
class GroveManager {
 public:
  virtual ~GroveManager() = default;

  // Propagate a tombstone for [key] to ALL derived trees in the grove.
  // Called by MymBroker::Delete() after deleting from the base CF.
  virtual Status PropagateDelete(std::string_view key) = 0;

  // Returns the names of all derived column families registered with this
  // grove instance.  Used by the scheduler to build DeferredCFs().
  virtual std::vector<std::string> DerivedCFNames() const = 0;
};

// ── DeferCallback ─────────────────────────────────────────────────────────────
//
// Invoked when the admission / scheduling layer decides to defer one or more
// transforms to a future compaction.
//
// The RocksDB adapter calls SchedulePendingCompaction() on the affected CFs
// (requires DBImpl* access, which is why this is behind an interface).
//
class DeferCallback {
 public:
  virtual ~DeferCallback() = default;

  // Schedule a follow-up compaction for the given set of column family names.
  // [file_numbers] is a hint — the set of SST file numbers whose transforms
  // were deferred; the engine may use this to narrow the compaction range.
  virtual Status ScheduleDeferred(
      const std::vector<std::string>& cf_names,
      const std::vector<uint64_t>&    file_numbers) = 0;

  // No-op default implementation so adapters can opt out during bring-up.
  // Override this in production adapters.
  static DeferCallback* Noop();
};

// ── EpochStore ────────────────────────────────────────────────────────────────
//
// Persists and retrieves TransformEpochTracker state for each SST file.
//
// The RocksDB adapter stores epoch records in the SST file's table properties
// (via a TablePropertiesCollector).  Another engine could use a sidecar file
// or a metadata column family.
//
class EpochStore {
 public:
  virtual ~EpochStore() = default;

  // Load the epoch tracker for [file_id] into [*out].
  // Returns Status::OK() if found, Status::Error("not found") if the SST has
  // no epoch record yet (first-time compaction).
  virtual Status Load(uint64_t             file_id,
                      TransformEpochTracker* out) const = 0;

  // Persist the epoch tracker for [file_id].
  virtual Status Save(uint64_t                    file_id,
                      const TransformEpochTracker& tracker) = 0;

  // Remove the epoch record for [file_id] (called when an SST is deleted).
  virtual Status Evict(uint64_t file_id) { (void)file_id; return Status::OK(); }
};

// ── No-op implementations (for testing / bring-up) ───────────────────────────
//
// Concrete stubs so unit tests can instantiate libmycelium classes without a
// full engine adapter.  These are header-only to avoid adding a .cc file to
// the scaffold.

class NullCompactionWriter final : public CompactionWriter {
 public:
  explicit NullCompactionWriter(size_t dest_count) : n_(dest_count) {}
  Status EmitKV(size_t, std::string_view, std::string_view) override {
    return Status::OK();
  }
  size_t DestCount() const override { return n_; }
 private:
  size_t n_;
};

class NullGroveManager final : public GroveManager {
 public:
  Status PropagateDelete(std::string_view) override { return Status::OK(); }
  std::vector<std::string> DerivedCFNames() const override { return {}; }
};

class NullDeferCallback final : public DeferCallback {
 public:
  Status ScheduleDeferred(const std::vector<std::string>&,
                          const std::vector<uint64_t>&) override {
    return Status::OK();
  }
};

inline DeferCallback* DeferCallback::Noop() {
  static NullDeferCallback instance;
  return &instance;
}

}  // namespace mycelium
