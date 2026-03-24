// compaction_metrics_listener.cc

#include "db/compaction_metrics_listener.h"

#include <chrono>
#include <iomanip>
#include <iostream>

namespace ycsbc {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

CompactionMetricsListener::CompactionMetricsListener(
    const std::string& csv_path,
    std::shared_ptr<rocksdb::Statistics> statistics)
    : csv_path_(csv_path), statistics_(std::move(statistics)) {
  out_.open(csv_path_, std::ios::out | std::ios::trunc);
  if (!out_.is_open()) {
    std::cerr << "[CompactionMetricsListener] WARNING: could not open "
              << csv_path_ << " for writing.\n";
  }
}

CompactionMetricsListener::~CompactionMetricsListener() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (out_.is_open()) {
    out_.flush();
    out_.close();
  }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

uint64_t CompactionMetricsListener::NowMicros() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

void CompactionMetricsListener::WriteHeader() {
  // timestamp_us       — wall-clock time the job completed (µs since epoch)
  // cf_name            — column family that was compacted
  // job_id             — RocksDB-assigned job id (unique within thread)
  // base_input_level   — smallest input level (L0 = 0)
  // output_level       — output level
  // elapsed_micros     — wall-clock duration of the job (µs)
  // cpu_micros         — CPU time consumed by compaction threads (µs)
  // cpu_utilization    — cpu_micros / elapsed_micros  (fraction of one core;
  //                      values > 1.0 indicate multi-threaded subcompactions)
  // total_input_bytes  — uncompressed bytes read
  // total_output_bytes — uncompressed bytes written
  // write_amp          — total_output_bytes / total_input_bytes
  // num_input_files    — SST files read
  // num_output_files   — SST files written
  // num_input_records  — KV pairs read
  // num_output_records — KV pairs written (after GC / key-drop)
  // is_manual          — 1 if triggered manually, 0 otherwise
  // is_full            — 1 if full compaction, 0 otherwise
  // pending_compaction_bytes — rocksdb.estimate-pending-compaction-bytes (bytes)
  // num_running_compactions  — rocksdb.num-running-compactions at event time
  // stall_micros_cumulative  — cumulative STALL_MICROS ticker value
  out_ << "timestamp_us"
       << ",cf_name"
       << ",job_id"
       << ",base_input_level"
       << ",output_level"
       << ",elapsed_micros"
       << ",cpu_micros"
       << ",cpu_utilization"
       << ",total_input_bytes"
       << ",total_output_bytes"
       << ",write_amp"
       << ",num_input_files"
       << ",num_output_files"
       << ",num_input_records"
       << ",num_output_records"
       << ",is_manual"
       << ",is_full"
       << ",pending_compaction_bytes"
       << ",num_running_compactions"
       << ",stall_micros_cumulative"
       << "\n";
  out_.flush();
  header_written_ = true;
}

// ---------------------------------------------------------------------------
// Primary callback
// ---------------------------------------------------------------------------

void CompactionMetricsListener::OnCompactionCompleted(
    rocksdb::DB* db,
    const rocksdb::CompactionJobInfo& ci) {

  // Skip jobs that failed — their stats may be partial.
  if (!ci.status.ok()) return;

  const rocksdb::CompactionJobStats& s = ci.stats;

  // ── Derived metrics ──────────────────────────────────────────────────────

  // CPU utilization: fraction of one core consumed for the job's duration.
  // A subcompaction spanning N threads can yield values > 1.0 — that is
  // correct and expected. Divide by max_subcompactions to normalise if needed.
  const double cpu_utilization =
      (s.elapsed_micros > 0)
          ? static_cast<double>(s.cpu_micros) /
                static_cast<double>(s.elapsed_micros)
          : 0.0;

  // Write amplification: bytes out / bytes in at the SST level.
  const double write_amp =
      (s.total_input_bytes > 0)
          ? static_cast<double>(s.total_output_bytes) /
                static_cast<double>(s.total_input_bytes)
          : 0.0;

  // ── DB-level properties (polled at event time) ───────────────────────────
  // These calls are safe from the compaction callback thread.

  uint64_t pending_compaction_bytes = 0;
  if (db) {
    db->GetIntProperty(rocksdb::DB::Properties::kEstimatePendingCompactionBytes,
                       &pending_compaction_bytes);
  }

  uint64_t num_running_compactions = 0;
  if (db) {
    db->GetIntProperty(rocksdb::DB::Properties::kNumRunningCompactions,
                       &num_running_compactions);
  }

  // ── Statistics ticker ────────────────────────────────────────────────────

  uint64_t stall_micros = 0;
  if (statistics_) {
    stall_micros = statistics_->getTickerCount(rocksdb::STALL_MICROS);
  }

  // ── Write CSV row ────────────────────────────────────────────────────────

  const uint64_t now_us = NowMicros();

  {
    std::lock_guard<std::mutex> lk(mutex_);

    if (!out_.is_open()) return;
    if (!header_written_) WriteHeader();

    out_ << now_us
         << "," << ci.cf_name
         << "," << ci.job_id
         << "," << ci.base_input_level
         << "," << ci.output_level
         << "," << s.elapsed_micros
         << "," << s.cpu_micros
         << "," << std::fixed << std::setprecision(6) << cpu_utilization
         << "," << s.total_input_bytes
         << "," << s.total_output_bytes
         << "," << std::fixed << std::setprecision(4) << write_amp
         << "," << s.num_input_files
         << "," << s.num_output_files
         << "," << s.num_input_records
         << "," << s.num_output_records
         << "," << (s.is_manual_compaction ? 1 : 0)
         << "," << (s.is_full_compaction   ? 1 : 0)
         << "," << pending_compaction_bytes
         << "," << num_running_compactions
         << "," << stall_micros
         << "\n";

    // Flush after every row: experiments may be interrupted early and we
    // don't want to lose data that was captured.
    out_.flush();
  }

  event_count_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace ycsbc
