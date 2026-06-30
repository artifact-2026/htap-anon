// compaction_metrics_listener.h
//
// EventListener subclass that captures per-compaction-job metrics and writes
// them to a structured CSV file for CPU slack characterization experiments.
//
// Usage:
//   auto stats = rocksdb::CreateDBStatistics();
//   options.statistics = stats;
//   auto listener = std::make_shared<CompactionMetricsListener>(
//       "/path/to/compaction_metrics.csv", stats);
//   options.listeners.push_back(listener);
//
// One row is written per completed compaction job. The CSV is flushed after
// each row so that data is recoverable even if the process is killed.
//
// Thread safety: OnCompactionCompleted may be called concurrently from
// multiple background compaction threads. All CSV writes are serialised
// via an internal mutex.

#pragma once

#include <atomic>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

#include <rocksdb/db.h>
#include <rocksdb/listener.h>
#include <rocksdb/statistics.h>

namespace ycsbc {

class CompactionMetricsListener : public rocksdb::EventListener {
 public:
  // @param csv_path    Path to the output CSV file (created/truncated on open).
  // @param statistics  The rocksdb::Statistics object attached to the DB.
  //                    Pass nullptr to skip stall_micros reporting.
  explicit CompactionMetricsListener(
      const std::string& csv_path,
      std::shared_ptr<rocksdb::Statistics> statistics = nullptr);

  ~CompactionMetricsListener() override;

  // Called by RocksDB on the compaction thread after a job completes.
  void OnCompactionCompleted(rocksdb::DB* db,
                             const rocksdb::CompactionJobInfo& ci) override;

  // Total number of compaction events recorded since construction.
  uint64_t EventCount() const { return event_count_.load(std::memory_order_relaxed); }

 private:
  // Writes the CSV header line. Must be called with mutex_ held.
  void WriteHeader();

  // Returns microseconds since the Unix epoch.
  static uint64_t NowMicros();

  const std::string csv_path_;
  std::shared_ptr<rocksdb::Statistics> statistics_;

  mutable std::mutex mutex_;
  std::ofstream out_;
  bool header_written_ = false;

  std::atomic<uint64_t> event_count_{0};
};

}  // namespace ycsbc
