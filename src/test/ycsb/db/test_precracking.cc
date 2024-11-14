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
        rocksdb::InputOutputDataType inputType = ycsbc::DBHelper::mapStringToDataType(props.GetProperty("inputdatatype", "PROTOBUF"));
        rocksdb::InputOutputDataType outputType = ycsbc::DBHelper::mapStringToDataType(props.GetProperty("outputdatatype", "PROTOBUF"));
        SetOptions(dbfilename, levels, fieldcount, bootstrap, inputType, outputType);

        std::vector<rocksdb::ColumnFamilyDescriptor> column_family_descriptors;
        GetColumnFamilyDescriptors(dbname, column_family_descriptors);
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
    int RocksdbColumnStrawman::Read(const std::string &table, const std::string &key, const std::set<std::string> *fields,
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
                          const std::string &end_key, const std::set<std::string> *fields,
                          const std::string &req_dist, bool index_access,
                          std::vector<std::string> &result) 
    {
        result.clear();
        int searched = 0;
        auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandles_[table+"_colgrp_0"]);
        it->Seek(begin_key);
        while (it->Valid() && searched < 25) {
            result.push_back(it->value().ToString());
            
            it->Next();
            searched++;
        }
        if (result.size() > 0) {
            return 0;
        }
        return 1;
    }

    int RocksdbColumnStrawman::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        rocksdb::Status s;
        
        data::Row row;
        row.ParseFromString(values);
        int grp_size = row.columns_size()/8;
        for (int i = 0; i < row.columns_size()-1; i++) {
            std::string serializedColumn;
            data::Row splitCols;

            for (int j=0; j < grp_size; j++) {
                if (i+j >= row.columns_size()) {
                    break;
                }
                splitCols.add_columns(row.columns(i+j));
            }
            splitCols.SerializeToString(&serializedColumn);
            
            if (!serializedColumn.empty()) {
                s = rocksdb_->Put(write_options_,
                                  cfhandles_[table+"_colgrp_"+std::to_string(i/2)],
                                  key,
                                  serializedColumn);
            }

            i += grp_size - 1;
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

    void RocksdbColumnStrawman::SetOptions(const char *dbfilename, int levels, int fieldcount, bool logging,
                rocksdb::InputOutputDataType inputDataType, rocksdb::InputOutputDataType outputDataType)
    {
        if (!logging) {
            options_.info_log_level = rocksdb::InfoLogLevel::FATAL_LEVEL;
        }

        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;
        options_.max_open_files = -1;

        options_.num_levels = levels;
        options_.num_columns = fieldcount;
        options_.SetTransformerType(rocksdb::TransformerType::NOTRANSFORMATION);
        options_.SetInputOutputDataType(inputDataType, outputDataType);

        options_.write_buffer_size = 256 * 1024 * 1024;
        options_.max_write_buffer_number = 4;
        options_.level0_file_num_compaction_trigger = 10;
        options_.level0_slowdown_writes_trigger = 20;
        options_.level0_stop_writes_trigger = 36;
        options_.max_background_flushes = 2;
        options_.max_background_compactions = 10;
        options_.compression = rocksdb::kNoCompression;
        options_.table_factory.reset(rocksdb::NewBlockBasedTableFactory(
                rocksdb::BlockBasedTableOptions{
                .block_cache = rocksdb::NewLRUCache(512 * 1024 * 1024)}));

        write_options_.disableWAL = true;

        /*
        options_.IncreaseParallelism(16);
        options_.target_file_size_base = 67108864;
        options_.use_direct_reads = true;
        options_.use_direct_io_for_flush_and_compaction = true;
        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_cache = nullptr;  // Disable the block cache
        options_.table_factory = std::shared_ptr<rocksdb::TableFactory>(rocksdb::NewBlockBasedTableFactory(table_options));
        */
    }

    void RocksdbColumnStrawman::GetColumnFamilyDescriptors(const std::string& dbname,
                    std::vector<rocksdb::ColumnFamilyDescriptor>& column_families)
    {
        int splits = 27;
        
        for (int i = 0; i < splits; i++) {
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
