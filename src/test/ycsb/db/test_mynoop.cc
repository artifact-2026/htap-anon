#include <nlohmann/json.hpp>
#include "core/core_workload.h"
#include "test_mynoop.h"
#include "lib/coding.h"
#include "transformer/mynooper.h"

using namespace std;

namespace ycsbc {
    TestMynoop::TestMynoop(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        int levels = utils::StrToInt(props.GetProperty("levels", "6"));
        int fieldcount = utils::StrToInt(props.GetProperty("fieldcount", "1"));
        inputType_ = props.GetProperty("inputdataformat", "protobuf");
        outputType_ = props.GetProperty("outputdataformat", "flatbuffers");
        columnDataType_ = props.GetProperty("columndatatype", "numeric");

        SetOptions(props, false, levels, fieldcount);
        options_.transformers.push_back(new rocksdb::Mynooper());

        std::vector<rocksdb::ColumnFamilyDescriptor> column_family_descriptors;
        GetColumnFamilyDescriptors(dbname, column_family_descriptors);
        std::vector<rocksdb::ColumnFamilyHandle*> cf_handles;

        if (bootstrap) {
            rocksdb::Status s = rocksdb::DB::Open(options_, 
                                              dbfilename,
                                              &rocksdb_);
            if (!s.ok()){
                std::cerr<<"Can't open mynoop "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }

            s = rocksdb_->CreateColumnFamilies(column_family_descriptors, &cf_handles);
            s = rocksdb_->AddTransformingDestinationCfds(dbname, false, false, false, false, 0);
            if (!s.ok()){
                std::cerr<<"Creating column families for mynoop db ran into error "<<s.ToString()<<std::endl;
                exit(0);
            }
        } else {
            column_family_descriptors.push_back(rocksdb::ColumnFamilyDescriptor(
                    rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions(options_)));
            rocksdb::Status s = rocksdb::DB::Open(options_,
                                              dbfilename,
                                              column_family_descriptors,
                                              &cf_handles,
                                              &rocksdb_);
            if (!s.ok()){
                std::cerr<<"Can't open mynoop "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }

            s = rocksdb_->AddTransformingDestinationCfds(dbname, false, false, false, false, 0);
            if (!s.ok()){
                std::cerr<<"Creating column families for mynoop db ran into error "<<s.ToString()<<std::endl;
                exit(0);
            }
        }
        BuildColumnFamilyHandles(column_family_descriptors, cf_handles);
    }
        
    void TestMynoop::GetColumnFamilyDescriptors(const std::string& dbname,
                    std::vector<rocksdb::ColumnFamilyDescriptor>& column_families)
    {
        options_.level0_file_num_compaction_trigger = 1;
        options_.compaction_pri = rocksdb::kMinOverlappingRatio;
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(
            dbname, rocksdb::ColumnFamilyOptions(options_)));
        
        options_.SetTransformerType(rocksdb::TransformerType::NOTRANSFORMATION);
        options_.level0_file_num_compaction_trigger = 4;
        options_.compaction_pri = rocksdb::kByCompensatedSize;
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(
            dbname+"_identity_cf", rocksdb::ColumnFamilyOptions(options_)));
    }

    /*
    * Read is for point query over all columns
    */
    int TestMynoop::Read(const std::string &table, const std::string &key, const std::set<std::string> *fields,
                      const std::string &req_dist, bool index_access, std::string &result) 
    {
        rocksdb::Status s;

        if (req_dist == "leastrecent") {
            s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table+"_identity_cf"], key, &result);
            if (result != "") {
                return 0;
            }
        } else {
            s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table], key, &result);
            if (result != "") {
                if (fields != nullptr) {
                    if (inputType_ == "json") {
                        nlohmann::json parsedJson = nlohmann::json::parse(result);
                    } else {
                        data::Row row;
                        row.ParseFromString(result);
                    }
                }
                return 0;
            }
            s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table+"_identity_cf"], key, &result);
            if (result != "") {
                return 0;
            }
        }

        return 1;
    }

    int TestMynoop::Scan(const std::string &table, const std::string &begin_key,
                          const std::string &end_key, const std::set<std::string> *fields,
                          const std::string &req_dist, bool index_access,
                          std::vector<std::string> &result) 
    {
        uint64_t sum = 0;
        
        auto itt = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandles_[table]);
        itt->Seek(begin_key);
        while (itt->Valid() && result.size() < 100) {
            if (fields != nullptr) {
                if (inputType_ == "protobuf") {
                    data::Row row;
                    row.ParseFromString(itt->value().ToString());
                    sum += std::stoi(row.columns(0));
                } else {
                    nlohmann::json parsedJson = nlohmann::json::parse(itt->value().ToString());
                    sum += std::stoi(parsedJson["field0"].get<std::string>());
                }
            }
            result.push_back(itt->value().ToString());
            itt->Next();
        }
        if (result.size() >= 100) {
            return 0;
        }

        auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandles_[table+"_identity_cf"]);
        it->Seek(begin_key);
        while (it->Valid() && result.size() < 100) {
            if (fields != nullptr) {
                if (inputType_ == "protobuf") {
                    data::Row row;
                    row.ParseFromString(it->value().ToString());
                    sum += std::stoi(row.columns(0));
                } else {
                    nlohmann::json parsedJson = nlohmann::json::parse(it->value().ToString());
                    sum += std::stoi(parsedJson["field0"].get<std::string>());
                }
            }
            result.push_back(it->value().ToString());
            it->Next();
        }
        
        if (result.size() >= 100) {
            return 0;
        }
        return 1;
    }

    int TestMynoop::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        rocksdb::Status s = rocksdb_->Put(write_options_, cfhandles_[table], key, values);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    int TestMynoop::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int TestMynoop::Delete(const std::string &table, const std::string &key)
    {
        rocksdb::Status s = rocksdb_->Delete(write_options_, cfhandles_[table], key);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    void TestMynoop::SetOptions(utils::Properties &props, bool logging, int levels, int fieldcount)
    {
        if (!logging) {
            options_.info_log_level = rocksdb::InfoLogLevel::FATAL_LEVEL;
        }
        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;
        options_.max_open_files = -1;
        options_.compaction_style = rocksdb::kCompactionStyleLevel;
        options_.disable_auto_compactions = false;

        options_.env->SetBackgroundThreads(20, rocksdb::Env::Priority::LOW);
        options_.env->SetBackgroundThreads(8, rocksdb::Env::Priority::HIGH);
        options_.max_background_compactions = 20;
        options_.max_background_flushes = 8;
        options_.max_subcompactions = 16;

        options_.write_buffer_size = 128 * 1024 * 1024;
        options_.max_write_buffer_number = 8;
        options_.max_bytes_for_level_base = 256 * 1024 * 1024;
        options_.target_file_size_base = 256 * 1024 * 1024;
        options_.level0_slowdown_writes_trigger = 50;
        options_.level0_stop_writes_trigger = 80;
        options_.IncreaseParallelism(24);
        options_.use_direct_reads = true;
        options_.use_direct_io_for_flush_and_compaction = true;
        
        options_.num_levels = levels;
        options_.num_columns = fieldcount;
        options_.column_data_type = columnDataType_;
        options_.SetTransformerType(rocksdb::TransformerType::MYNOOPER);
        options_.SetInputOutputDataType(ycsbc::DBHelper::mapStringToDataType(inputType_),
                                        ycsbc::DBHelper::mapStringToDataType(outputType_));

        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_cache = rocksdb::NewLRUCache(512 * 1024 * 1024);
        table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
        options_.table_factory = std::shared_ptr<rocksdb::TableFactory>(rocksdb::NewBlockBasedTableFactory(table_options));
    }

    void TestMynoop::BuildColumnFamilyHandles(std::vector<rocksdb::ColumnFamilyDescriptor> &column_family_descriptors,
                                                std::vector<rocksdb::ColumnFamilyHandle *> handles)
    {
        for (size_t i = 0; i < handles.size(); i++) {
            if (column_family_descriptors[i].name != rocksdb::kDefaultColumnFamilyName) {
                cfhandles_.insert({column_family_descriptors[i].name, handles[i]});
            }
        }
    }

}
