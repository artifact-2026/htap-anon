#include <nlohmann/json.hpp>
#include "core/core_workload.h"
#include "test_precracking.h"
#include "lib/coding.h"

using namespace std;

namespace ycsbc {
    RocksdbColumnStrawman::RocksdbColumnStrawman(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        noResults = 0;
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        int levels = utils::StrToInt(props.GetProperty("levels", "6"));
        int fieldcount = utils::StrToInt(props.GetProperty("fieldcount", "2"));
        inputType_ = props.GetProperty("inputdataformat", "protobuf");
        outputType_ = props.GetProperty("outputdataformat", "flatbuffers");
        columnDataType_ = props.GetProperty("columndatatype", "numeric");
        SetOptions(dbfilename, levels, fieldcount, true);

        std::vector<rocksdb::ColumnFamilyDescriptor> column_family_descriptors;
        GetColumnFamilyDescriptors(dbname, fieldcount, column_family_descriptors);
        std::vector<rocksdb::ColumnFamilyHandle*> cf_handles;

        if (bootstrap) {
            rocksdb::Status s = rocksdb::DB::Open(options_, 
                                              dbfilename,
                                              &rocksdb_);
            if (!s.ok()){
                std::cerr<<"Can't open precracking "<<dbfilename<<" "<<s.ToString()<<std::endl;
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
                std::cerr<<"Can't open precracking "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }
        }
        BuildColumnFamilyHandleMap(column_family_descriptors, cf_handles);
    }

    /*
    * Read is for point query over all columns
    */
    int RocksdbColumnStrawman::Read(const std::string &table, const std::string &key, const std::set<int> *fields,
                      const std::string &req_dist, bool index_access, std::string &result) 
    {
        rocksdb::Status s;
        result = "";
        if (fields == nullptr) {
            for (int i = 0; i < 8; i++) {
                std::string value;
                s = rocksdb_->Get(rocksdb::ReadOptions(),
                                           cfhandles_[table+"_colgrp_"+std::to_string(i)],
                                           key,
                                           &value);

                result += value;
            }
        } else {
            s = rocksdb_->Get(rocksdb::ReadOptions(),
                              cfhandles_[table+"_colgrp_0"],
                              key,
                              &result);
        }
        if (result != "") {
            return 0;
        }
        return 1;
    }

    int RocksdbColumnStrawman::Scan(const std::string &table, const std::string &begin_key,
                          const std::string &end_key, const std::set<int> *fields,
                          const std::string &req_dist, bool index_access,
                          std::vector<std::string> &result) 
    {
        int searched = 0;
        if (fields == nullptr) {
            for (int i = 0; i < 8; i++) {
                std::string cfname = table+"_colgrp_"+std::to_string(i);
                auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandles_[cfname]);
                it->Seek(begin_key);
                searched = 0;
                while (it->Valid() && searched < 100) {
                    result.push_back(it->value().ToString());
                    searched++;
                }
            }
            if (result.size() >= 800) {
                return 0;
            }
        } else {
            auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandles_[table+"_colgrp_0"]);
            it->Seek(begin_key);
            while (it->Valid() && searched < 100) {
                uint64_t sum = 0;
                if (fields != nullptr) {
                    if (inputType_ == "protobuf") {
                        data::ByteRow row;
                        row.ParseFromString(it->value().ToString());
                        sum += std::stoi(row.values(0).value());
                    } else {
                        nlohmann::json parsedJson = nlohmann::json::parse(it->value().ToString());
                        sum += std::stoi(parsedJson["col0"].get<std::string>());
                    }
                }
                result.push_back(it->value().ToString());
                it->Next();
                searched++;
            }
            if (result.size() > 100) {
                return 0;
            }
        }
        return 1;
    }

    int RocksdbColumnStrawman::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        rocksdb::Status s;
        
        if (inputType_ == "protobuf") {
            data::ByteRow row;
            if (!row.ParseFromString(values)) {
                std::cout << "parsing value input into Protobuf schema had an error." << std::endl;
                return 1;
            }

            const int num_cols   = row.values_size();
            constexpr int kGroupSize = 1;
            const int num_groups = num_cols;  // 32 -> 8

            for (int g = 0; g < num_groups; ++g) {
                data::ByteRow groupRow;

                const int start = g * kGroupSize;
                const int end   = std::min(start + kGroupSize, num_cols);
                for (int i = start; i < end; ++i) {
                    const auto& src = row.values(i);
                    auto* c = groupRow.add_values();
                    c->set_value(src.value());
                }

                std::string serialized;
                serialized.reserve(groupRow.ByteSizeLong());   // optional, avoids reallocs
                groupRow.SerializeToString(&serialized);

                // write to CF: table + "_colgrp_" + group_index  (expects 8 CFs: 0..7)
                const std::string cfname = table + "_colgrp_" + std::to_string(g);
                auto it = cfhandles_.find(cfname);
                assert(it != cfhandles_.end());                // make sure the CF exists

                s = rocksdb_->Put(write_options_, it->second, key, serialized);
                if (!s.ok()) {
                    // handle/log error or break/return as appropriate for your code
                    break;
                }
            }
        } else {
            nlohmann::json parsedJson = nlohmann::json::parse(values);
            const size_t j_size = parsedJson.size();
            size_t grp_size = j_size/8;

            for (size_t i = 0; i < j_size; i++) {
                nlohmann::json jsonData;

                for (size_t j=0; j < grp_size; j++) {
                    if (i+j >= j_size) {
                        break;
                    }
                    auto fname = "col" + std::to_string(i+j);
                    jsonData[fname] = parsedJson.at(fname);
                }
                std::string cfname = table+"_colgrp_"+std::to_string(i/grp_size);
                auto it = cfhandles_.find(cfname);
                if (it == cfhandles_.end() || it->second == nullptr) {
                    fprintf(stderr, "Missing/NULL CF handle for name='%s'\n", cfname.c_str());
                    abort();
                }
                s = rocksdb_->Put(write_options_,
                                  it->second,
                                  key,
                                  jsonData.dump());
                
                i += grp_size - 1;
            }
        }
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    int RocksdbColumnStrawman::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int RocksdbColumnStrawman::Delete(const std::string &table, const std::string &key)
    {
        rocksdb::Status s;
        for (int i = 0; i < 8; i++) {
            s = rocksdb_->Delete(write_options_,
                                 cfhandles_[table+"_colgrp_"+std::to_string(i)],
                                 key);
        }
        
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    void RocksdbColumnStrawman::SetOptions(const char *dbfilename, int levels, int fieldcount, bool logging)
    {
        if (!logging) {
            options_.info_log_level = rocksdb::InfoLogLevel::FATAL_LEVEL;
        }

        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;
        options_.max_open_files = -1;
        // Background thread pool: covers both compaction and flush.
        options_.max_background_jobs = 8;
        // Allow up to 4 parallel sub-compactions per compaction job on NVMe.
        options_.max_subcompactions = 4;

        options_.num_levels = levels;
        options_.num_columns = fieldcount;

        // 64 MB memtable (RocksDB default); keep 4 in memory before stalling.
        options_.write_buffer_size = 64 * 1024 * 1024;
        options_.max_write_buffer_number = 4;
        // L0 triggers (RocksDB defaults; trigger=4×64MB=256MB = max_bytes_for_level_base).
        //options_.level0_file_num_compaction_trigger = 4;
        //options_.level0_slowdown_writes_trigger = 20;
        //options_.level0_stop_writes_trigger = 36;
        options_.use_direct_reads = true;
        options_.use_direct_io_for_flush_and_compaction = true;

        //options_.target_file_size_base = 64 * 1024 * 1024;
        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_cache = rocksdb::NewLRUCache(512 * 1024 * 1024);
        table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
        options_.table_factory = std::shared_ptr<rocksdb::TableFactory>(rocksdb::NewBlockBasedTableFactory(table_options));
    }

    void RocksdbColumnStrawman::GetColumnFamilyDescriptors(const std::string& dbname,
                    int num_groups,
                    std::vector<rocksdb::ColumnFamilyDescriptor>& column_families)
    {
        for (int i = 0; i < num_groups; i++) {
            std::string cf_name = dbname + "_colgrp_" + std::to_string(i);
            column_families.push_back(rocksdb::ColumnFamilyDescriptor(cf_name, rocksdb::ColumnFamilyOptions(options_)));
        }
    }

    void RocksdbColumnStrawman::BuildColumnFamilyHandleMap(std::vector<rocksdb::ColumnFamilyDescriptor>& column_family_descriptors,
                            std::vector<rocksdb::ColumnFamilyHandle*> handles)
    {
        for (size_t i = 0; i < handles.size(); i++) {
            if (column_family_descriptors[i].name == rocksdb::kDefaultColumnFamilyName) {
                continue;
            }
            cfhandles_.insert({column_family_descriptors[i].name, handles[i]});
            handleList_.push_back(handles[i]);
        }
    }

    std::set<int> RocksdbColumnStrawman::GetQueryingHandles(std::set<std::string> fields) {
        std::set<int> fieldpositions;
        for (auto field : fields) {
            int pos = 0;
            for (size_t i = 5; i < field.size(); i++) {
                pos = pos*10 + field[i] - '0';
            }
            fieldpositions.insert(pos/2);
        }
        return fieldpositions;
    }

}
