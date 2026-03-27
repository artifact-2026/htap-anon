#include <nlohmann/json.hpp>
#include "core/core_workload.h"
#include "test_rocks_db.h"
#include "lib/coding.h"

#include <rocksdb/statistics.h>

using namespace std;

namespace ycsbc {
    TestRocksDB::TestRocksDB(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        noResults = 0;
        int levels = utils::StrToInt(props.GetProperty("levels", "7"));
        int fieldcount = utils::StrToInt(props.GetProperty("fieldcount", "1"));
        columnDataType_ = props.GetProperty("columndatatype", "numeric");
        SetOptions(props, true, levels, fieldcount);

        // Attach the compaction metrics listener when a CSV output path is given.
        // To enable, set "metrics_output=/path/to/compaction_metrics.csv" in
        // the workload .spec file or on the command line (-P key=value).
        const std::string metrics_path = props.GetProperty("metrics_output", "");
        if (!metrics_path.empty()) {
            metrics_listener_ = std::make_shared<CompactionMetricsListener>(
                metrics_path, dbstats_);
            options_.listeners.push_back(metrics_listener_);
            std::cout << "[CompactionMetricsListener] writing to: "
                      << metrics_path << "\n";
        }

        std::vector<rocksdb::ColumnFamilyDescriptor> column_family_descriptors;
        GetColumnFamilyDescriptors(dbname, column_family_descriptors);

        if (bootstrap) {
            rocksdb::Status s = rocksdb::DB::Open(options_, 
                                              dbfilename,
                                              &rocksdb_);

            if (!s.ok()){
                std::cerr<<"Can't open rocksdb "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }

            s = rocksdb_->CreateColumnFamilies(column_family_descriptors, &cfhandles_);
        } else {
            column_family_descriptors.push_back(rocksdb::ColumnFamilyDescriptor(
                    rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions(options_)));
            rocksdb::Status s = rocksdb::DB::Open(options_,
                                              dbfilename,
                                              column_family_descriptors,
                                              &cfhandles_,
                                              &rocksdb_);
            if (!s.ok()){
                std::cerr<<"Can't open rocksdb "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }
        }
        BuildColumnFamilyHandles(column_family_descriptors, cfhandles_);
    }

    void TestRocksDB::GetColumnFamilyDescriptors(const std::string &dbname, std::vector<rocksdb::ColumnFamilyDescriptor>& column_families)
    {
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(
                dbname, rocksdb::ColumnFamilyOptions(options_)));
    }

    /*
    * Read is for point query over all columns
    */
    int TestRocksDB::Read(const std::string &table, const std::string &key, const std::set<int> *fields,
                      const std::string &req_dist, bool index_access, std::string &result) 
    {
        if (index_access) {
            auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandle_);
            it->SeekToFirst();
            while (it->Valid()) {
                /*nlohmann::json parsedJson = nlohmann::json::parse(it->value().ToString());
                if (parsedJson["field0"] == key) {
                    result = it->value().ToString();
                    return 0;
                }*/
                data::ByteRow row;
                row.ParseFromString(it->value().ToString());
                if (row.values(4).value() == key) {
                    result = it->value().ToString();
                    return 0;
                }
                it->Next();
            }
        } else {
            rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandle_, key, &result);
            if (s.ok()) {
                return 0;
            }
        }
        
        return 1;
    }

    int TestRocksDB::Scan(const std::string &table, const std::string &begin_key,
                          const std::string &end_key, const std::set<int> *fields,
                          const std::string &req_dist, bool index_access,
                          std::vector<std::string> &result) 
    {
        if (index_access) {
            auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandle_);
            it->SeekToFirst();
            while (it->Valid()) {
                nlohmann::json parsedJson = nlohmann::json::parse(it->value().ToString());
                if (parsedJson["field0"].get<std::string>() >= begin_key && parsedJson["field0"].get<std::string>() < end_key) {
                    result.push_back(it->value().ToString());
                }
                /*data::Row row;
                row.ParseFromString(it->value().ToString());
                if (row.columns(1).value() >= begin_key && row.columns(1).value() < end_key) {
                    result.push_back(it->value().ToString());
                }*/
                it->Next();
            }
            if (result.size() > 0) {
                std::vector<std::string> rowvals;
                for (auto res : result) {
                    std::string rowval;
                    rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandle_, res, &rowval);

                    if (s.ok() && rowval != "") {
                        rowvals.push_back(rowval);
                    }
                }

                if (rowvals.size() > 0) {
                    return 0;
                }
            }
        } else {
            rocksdb::ReadOptions ro;
            std::unique_ptr<rocksdb::Iterator> it(rocksdb_->NewIterator(ro, cfhandle_));
            it->Seek(begin_key);

            while (it->Valid() && result.size() < 100) {
                result.emplace_back(it->value().ToString());
                it->Next();
            }
            if (result.size() >= 100) {
                return 0;
            }
        }
        
        return 1;
    }

    int TestRocksDB::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        rocksdb::Status s = rocksdb_->Put(write_options_, cfhandle_, key, values);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    int TestRocksDB::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int TestRocksDB::Delete(const std::string &table, const std::string &key)
    {
        rocksdb::Status s = rocksdb_->Delete(write_options_, cfhandle_, key);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    void TestRocksDB::SetOptions(utils::Properties &props, bool logging, int levels, int fieldcount)
    {
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
        options_.IncreaseParallelism(32);

        // Allow up to 8 parallel sub-compactions per compaction job.
        // With 32 background threads and a fast NVMe, this keeps each L0→L1
        // job shorter so the L0 file count stays low between compactions.
        options_.max_subcompactions = 8;

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
        options_.use_direct_reads = false;
        options_.use_direct_io_for_flush_and_compaction = false;

        // Enable per-compaction I/O timing so CompactionJobStats.file_write_nanos
        // and related fields are populated (used by CompactionMetricsListener).
        options_.report_bg_io_stats = true;

        // Create a Statistics object so STALL_MICROS and other tickers are
        // tracked. The same shared_ptr is passed to CompactionMetricsListener.
        dbstats_ = rocksdb::CreateDBStatistics();
        options_.statistics = dbstats_;
        options_.compression = rocksdb::kNoCompression;

        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_cache = rocksdb::NewLRUCache(512 * 1024 * 1024);
        table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
        options_.table_factory = std::shared_ptr<rocksdb::TableFactory>(rocksdb::NewBlockBasedTableFactory(table_options));
    }

    void TestRocksDB::BuildColumnFamilyHandles(std::vector<rocksdb::ColumnFamilyDescriptor>& column_family_descriptors,
                                                std::vector<rocksdb::ColumnFamilyHandle*>& handles)
    {
        for (size_t i = 0; i < handles.size(); i++) {
            if (column_family_descriptors[i].name == rocksdb::kDefaultColumnFamilyName) {
                default_cf_ = handles[i];
            } else {
                cfhandle_ = handles[i];
            }
        }
    }
}
