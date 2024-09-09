#include <iostream>
#include <cmath>
#include <queue>
#include "core/core_workload.h"
#include "test_indexing.h"
#include "lib/coding.h"
#include "transformer/distributor.h"

using namespace std;

namespace ycsbc {
    Indexing::Indexing(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        noResults = 0;
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        int levels = utils::StrToInt(props.GetProperty("levels", "7"));
        int fieldcount = utils::StrToInt(props.GetProperty("fieldcount", "1"));
        SetOptions(dbfilename, bootstrap, levels, fieldcount);

        std::vector<rocksdb::DeriveFuncData*> deriveFuncs;
        deriveFuncs.push_back(CreateIndexer(std::vector<int>(3)));
        options_.transformers.push_back(new rocksdb::Augmenter(deriveFuncs));

        std::vector<rocksdb::ColumnFamilyDescriptor> column_family_descriptors;
        GetColumnFamilyDescriptors(dbname, column_family_descriptors);
        std::vector<rocksdb::ColumnFamilyHandle*> cf_handles;

        if (bootstrap) {
            rocksdb::Status s = rocksdb::DB::Open(options_, 
                                                  dbfilename,
                                                  &rocksdb_);
            if (!s.ok()){
                std::cerr<<"Can't open db "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }

            s = rocksdb_->CreateColumnFamilies(column_family_descriptors, &cf_handles);
            s = rocksdb_->AddTransformingDestinationCfds(dbname, false, false, true);
            if (!s.ok()){
                std::cerr<<"Creating column families ran into error "<<s.ToString()<<std::endl;
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
                std::cerr<<"Can't open db "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }
        }
        BuildColumnFamilyHandleMap(column_family_descriptors, cf_handles);
        rocksdb_->DisplayTransformingDestinationCfds();
    }

    /*
    * Read is for point query over all columns
    * "Indexing" db supports queries reading with value of a field in the row, in which case 
    * it will first issue a read with "table" being the index column family, get the result, 
    * which is the "key" to the original table; and then issue another read to the original 
    * table with the key passed in being the result from the previous read.
    */
    int Indexing::Read(const std::string &table, const std::string &key,
                        const std::set<std::string> *fields, std::string &result) 
    {
        /**
         * std::istringstream iss(data);
    size_t size;
    iss.read(reinterpret_cast<char*>(&size), sizeof(size));

    std::vector<std::string> vec(size);
    for (size_t i = 0; i < size; ++i) {
        size_t length;
        iss.read(reinterpret_cast<char*>(&length), sizeof(length));
        vec[i].resize(length);
        iss.read(&vec[i][0], length);
    }

    return vec;
         */
        auto it = cfhandles_.find(table);
        if (it != cfhandles_.end()) {
            rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(),
                                              it->second,
                                              key,
                                              &result);
        
            if (s.ok()) {
                return 0;
            }
        }
        return 1;
    }

    int Indexing::Scan(const std::string &table, const std::string &begin_key,
                          int32_t len, const std::set<std::string> *fields,
                          std::vector<std::string> &result) 
    {
        result.clear();
        auto it = rocksdb_->NewIterator(rocksdb::ReadOptions());
        it->Seek(begin_key);
        for (int i = 0; i < len && it->Valid(); i++) {
            std::string value = it->value().ToString();
	        result.push_back(value);      
            it->Next();
        }
        
        return result.size();
    }

    int Indexing::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        auto it = cfhandles_.find(table);
        if (it != cfhandles_.end()) {
            rocksdb::Status s = rocksdb_->Put(rocksdb::WriteOptions(),
                                          it->second,
                                          key,
                                          values);
            if (s.ok()) {
                return 0;
            }
        }
        return 1;
    }

    int Indexing::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int Indexing::Delete(const std::string &table, const std::string &key)
    {
        auto it = cfhandles_.find(table);
        if (it != cfhandles_.end()) {
            rocksdb::Status s = rocksdb_->Delete(rocksdb::WriteOptions(),
                                             it->second,
                                             key);
            if (s.ok()) {
                return 0;
            }
        }
        return 1;
    }

    void Indexing::SetOptions(const char *dbfilename, bool logging, int levels, int fieldcount)
    {
        if (!logging) {
            options_.info_log_level = rocksdb::InfoLogLevel::FATAL_LEVEL;
        }
        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;

        options_.num_levels = levels;
        options_.num_columns = fieldcount;
        options_.SetTransformerType(rocksdb::TransformerType::AUGMENTER);

        options_.IncreaseParallelism(16);
        options_.level0_slowdown_writes_trigger = 16;     
        options_.level0_stop_writes_trigger = 24;
        options_.max_open_files = -1;
        options_.level0_file_num_compaction_trigger = 8;

        options_.max_write_buffer_number = 3;
        options_.write_buffer_size = 67108864;
        options_.target_file_size_base = 67108864;

        options_.use_direct_reads = true;
        options_.use_direct_io_for_flush_and_compaction = true;
    }

    void Indexing::KeepOnlyRequestedFields(data::Row &row,
                    const std::set<std::string> *fields, data::Row &selectedColumns)
    {
        for (auto field : *fields) {
            for (int i = 0; i < row.columns_size(); i++) {
                if (row.columns(i).name().compare(field) == 0) {
                    data::Column* selectedColumn = selectedColumns.add_columns();
                    selectedColumn->set_name(row.columns(i).name());
                    selectedColumn->set_value(row.columns(i).value());
                    break;
                }
            }
        }
    }

    void Indexing::GetColumnFamilyDescriptors(const std::string& dbname, std::vector<rocksdb::ColumnFamilyDescriptor>& column_families)
    {
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(dbname, rocksdb::ColumnFamilyOptions(options_)));
        
        std::string index_name = dbname + "_derived_cf_0";
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(index_name, rocksdb::ColumnFamilyOptions(options_)));
    }

    void Indexing::BuildColumnFamilyHandleMap(std::vector<rocksdb::ColumnFamilyDescriptor>& column_family_descriptors,
                                              std::vector<rocksdb::ColumnFamilyHandle*> handles)
    {
        for (size_t i = 0; i < handles.size(); i++) {
            cfhandles_.insert({column_family_descriptors[i].name, handles[i]});
        }
    }

    rocksdb::DeriveFuncData* Indexing::CreateIndexer(std::vector<int> positions) {
        std::function<std::string(std::vector<std::string>&)> f = [&](std::vector<std::string> strs) -> std::string {
            if (strs.size() == 0) {
                return "";
            }
            if (strs.size() == 1) {
                return strs[0];
            }
            std::string ind = strs[0];
            for (size_t i = 1; i < strs.size(); i++) {
                ind += strs[i];
            }
            return ind;
        };
        return new rocksdb::DeriveFuncData(positions, f);
    }

}
