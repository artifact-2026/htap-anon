#include <nlohmann/json.hpp>
#include "core/core_workload.h"
#include "test_rocks_db.h"
#include "lib/coding.h"

using namespace std;

namespace ycsbc {
    TestRocksDB::TestRocksDB(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        noResults = 0;
        int levels = utils::StrToInt(props.GetProperty("levels", "6"));
        int fieldcount = utils::StrToInt(props.GetProperty("fieldcount", "1"));
        columnDataType_ = props.GetProperty("columndatatype", "numeric");
        SetOptions(props, false, levels, fieldcount);

        std::vector<rocksdb::ColumnFamilyDescriptor> column_family_descriptors;
        GetColumnFamilyDescriptors(dbname, column_family_descriptors);
        std::vector<rocksdb::ColumnFamilyHandle*> cf_handles;

        if (bootstrap) {
            rocksdb::Status s = rocksdb::DB::Open(options_, 
                                              dbfilename,
                                              &rocksdb_);

            if (!s.ok()){
                std::cerr<<"Can't open rocksdb "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }

            s = rocksdb_->CreateColumnFamilies(column_family_descriptors, &cf_handles);
        } else {
            column_family_descriptors.push_back(rocksdb::ColumnFamilyDescriptor(
                    rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions(options_)));
            rocksdb::Status s = rocksdb::DB::Open(options_,
                                              dbfilename,
                                              column_family_descriptors,
                                              &cf_handles,
                                              &rocksdb_);
            if (!s.ok()){
                std::cerr<<"Can't open rocksdb "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }
        }
        BuildColumnFamilyHandles(column_family_descriptors, cf_handles);
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
                data::Row row;
                row.ParseFromString(it->value().ToString());
                if (row.field5() == key) {
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
            auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandle_);
            it->Seek(begin_key);
            while (it->Valid() && result.size() < 100) {
                /*uint64_t sum = 0;
                if (fields != nullptr) {
                    if (inputType_ == "protobuf") {
                        data::Row row;
                        row.ParseFromString(it->value().ToString());
                        sum += std::stoi(row.columns(0));
                    } else {
                        nlohmann::json parsedJson = nlohmann::json::parse(it->value().ToString());
                        sum += std::stoi(parsedJson["field0"].get<std::string>());
                    }
                }*/
                
                result.push_back(it->value().ToString());
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
	    options_.env->SetBackgroundThreads(16, rocksdb::Env::Priority::LOW);
	    options_.env->SetBackgroundThreads(8, rocksdb::Env::Priority::HIGH);
	    options_.max_background_compactions = 16;
	    options_.max_background_flushes = 8;

	    options_.max_subcompactions = 16;

        options_.num_levels = levels;
        options_.num_columns = fieldcount;
        options_.SetTransformerType(rocksdb::TransformerType::NOTRANSFORMATION);

        options_.compaction_style = rocksdb::kCompactionStyleLevel;
        options_.write_buffer_size = 128 * 1024 * 1024;
        options_.max_write_buffer_number = 8;
        options_.max_bytes_for_level_base = 256 * 1024 * 1024;
        options_.target_file_size_base = 256 * 1024 * 1024;
        options_.level0_file_num_compaction_trigger = 4;
        options_.level0_slowdown_writes_trigger = 30;
        options_.level0_stop_writes_trigger = 64;

        options_.IncreaseParallelism(24);
        options_.use_direct_reads = true;
        options_.use_direct_io_for_flush_and_compaction = true;
        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_cache = rocksdb::NewLRUCache(512 * 1024 * 1024);
        table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
        options_.table_factory = std::shared_ptr<rocksdb::TableFactory>(rocksdb::NewBlockBasedTableFactory(table_options));
    }

    void TestRocksDB::BuildColumnFamilyHandles(std::vector<rocksdb::ColumnFamilyDescriptor> &column_family_descriptors,
                                                std::vector<rocksdb::ColumnFamilyHandle *> handles)
    {
        for (size_t i = 0; i < handles.size(); i++) {
            if (column_family_descriptors[i].name != rocksdb::kDefaultColumnFamilyName) {
                cfhandle_ = handles[i];
            }
        }
    }
}
