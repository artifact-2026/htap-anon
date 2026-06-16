#include "db_helper.h"
#include <rocksdb/table.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/statistics.h>

using namespace std;

namespace ycsbc {

    mycelium::InputOutputDataType DBHelper::mapStringToDataType(const std::string& dataType) {
        if (dataType == "json" || dataType == "JSON") return mycelium::InputOutputDataType::JSON;
        if (dataType == "protobuf" || dataType == "PROTOBUF") return mycelium::InputOutputDataType::PROTOBUF;
        if (dataType == "flatbuffers" || dataType == "FLATBUFFERS") return mycelium::InputOutputDataType::FLATBUFFERS;
        return mycelium::InputOutputDataType::UNKNOWN;
    }

    void DBHelper::SetOptions(rocksdb::Options& options_, bool logging, utils::Properties &props)
    {
        int levels = utils::StrToInt(props.GetProperty("levels", "7"));
        int fieldcount = utils::StrToInt(props.GetProperty("fieldcount", "1"));
        
        if (!logging) {
            options_.info_log_level = rocksdb::InfoLogLevel::FATAL_LEVEL;
        }

        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;
        options_.max_open_files = -1;

        // IncreaseParallelism sets max_background_jobs, configures the LOW
        // (compaction) and HIGH (flush) thread pools, and enables adaptive
        // write-thread yielding.  The explicit max_background_jobs line is
        // intentionally omitted — IncreaseParallelism is the single source of
        // truth for thread counts.
        // Set via workload spec: rocksdb_parallelism=<n>  (default: 32)
        int parallelism = utils::StrToInt(props.GetProperty("rocksdb_parallelism", "32"));
        options_.IncreaseParallelism(parallelism);

        // Allow up to 8 parallel sub-compactions per compaction job.
        // With 32 background threads and a fast NVMe, this keeps each L0→L1
        // job shorter so the L0 file count stays low between compactions.
        options_.max_subcompactions = 8;

        options_.num_levels = levels;
        options_.num_columns = fieldcount;

        options_.compaction_style = rocksdb::kCompactionStyleLevel;
        // 256 MB memtable — 4× the default.  At a typical write rate of
        // ~70 MB/s per thread, this means one thread creates an L0 file only
        // every ~3.6 s instead of every ~0.9 s with 64 MB.  Four threads
        // therefore produce L0 files at roughly the same rate as one thread
        // did with the 64 MB setting, keeping compaction from being
        // overwhelmed until a meaningfully higher thread count.
        options_.write_buffer_size = 256 * 1024 * 1024;
        // 8 write buffers: up to 2 GB of memtables in flight, giving the
        // flush pipeline enough slack to absorb bursts without stalling.
        options_.max_write_buffer_number = 8;
        // L0 triggers — at 256 MB/file, slowdown fires at 40×256MB = 10 GB L0
        // and the hard stop at 64×256MB = 16 GB L0.  This ensures the
        // steady-state window is never inside a stall at low thread counts.
        options_.level0_file_num_compaction_trigger = 4;
        options_.level0_slowdown_writes_trigger = 40;
        options_.level0_stop_writes_trigger = 64;
        // max_bytes_for_level_base must track write_buffer_size:
        //   recommended = write_buffer_size × level0_file_num_compaction_trigger
        //   = 256 MB × 4 = 1 GB
        // Without this, L1 is 4× too small for the new file sizes and every
        // L0→L1 compaction immediately triggers an L1→L2 cascade, negating
        // the benefit of the larger write buffer.
        options_.max_bytes_for_level_base = 1ull * 1024 * 1024 * 1024;
        options_.use_direct_reads = true;
        options_.use_direct_io_for_flush_and_compaction = true;

        // Enable per-compaction I/O timing so CompactionJobStats.file_write_nanos
        // and related fields are populated (used by CompactionMetricsListener).
        options_.report_bg_io_stats = true;

        // Create a Statistics object so STALL_MICROS and other tickers are
        // tracked. The same shared_ptr is passed to CompactionMetricsListener.
        options_.statistics = rocksdb::CreateDBStatistics();
        options_.compression = rocksdb::kNoCompression;

        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_cache = rocksdb::NewLRUCache(512 * 1024 * 1024);
        table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
        options_.table_factory = std::shared_ptr<rocksdb::TableFactory>(rocksdb::NewBlockBasedTableFactory(table_options));
    }

}
