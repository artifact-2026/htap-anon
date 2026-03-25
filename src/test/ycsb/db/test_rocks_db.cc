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
        // Background thread pool: covers both compaction and flush.
        // 8 is a good default for NVMe servers; increase if compaction lags.
        options_.max_background_jobs = 8;
        // Allow up to 4 parallel sub-compactions per compaction job on NVMe.
        options_.max_subcompactions = 4;

        options_.num_columns = fieldcount;

        options_.compaction_style = rocksdb::kCompactionStyleLevel;
        // 64 MB memtable (RocksDB default); keep 4 in memory before stalling.
        options_.write_buffer_size = 64 * 1024 * 1024;
        options_.max_write_buffer_number = 4;
        // L0 triggers (values below are RocksDB defaults; shown for visibility).
        // trigger=4: compaction starts at 4×64MB=256MB L0 (= max_bytes_for_level_base)
        // slowdown=20: write throttle at 20×64MB=1.28GB L0
        // stop=36:    hard write stop at 36×64MB=2.3GB L0
        //options_.level0_file_num_compaction_trigger = 4;
        //options_.level0_slowdown_writes_trigger = 20;
        //options_.level0_stop_writes_trigger = 36;

        options_.IncreaseParallelism(24);
        options_.use_direct_reads = true;
        options_.use_direct_io_for_flush_and_compaction = true;

        // Enable per-compaction I/O timing so CompactionJobStats.file_write_nanos
        // and related fields are populated (used by CompactionMetricsListener).
        options_.report_bg_io_stats = true;

        // Create a Statistics object so STALL_MICROS and other tickers are
        // tracked. The same shared_ptr is passed to CompactionMetricsListener.
        dbstats_ = rocksdb::CreateDBStatistics();
        options_.statistics = dbstats_;

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
