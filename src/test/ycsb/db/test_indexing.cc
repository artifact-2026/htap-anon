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
        int levels = utils::StrToInt(props.GetProperty("levels", "6"));
        int fieldcount = utils::StrToInt(props.GetProperty("fieldcount", "1"));
        rocksdb::InputOutputDataType inputType = ycsbc::DBHelper::mapStringToDataType(props.GetProperty("inputdatatype", "PROTOBUF"));
        rocksdb::InputOutputDataType outputType = ycsbc::DBHelper::mapStringToDataType(props.GetProperty("outputdatatype", "PROTOBUF"));
        SetOptions(dbfilename, bootstrap, levels, fieldcount, inputType, outputType);

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

            s = rocksdb_->AddTransformingDestinationCfds(dbname, false, false, true);
            if (!s.ok()){
                std::cerr<<"Column family creation for indexing ran into error "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }

        }
        BuildColumnFamilyHandleMap(column_family_descriptors, cf_handles);
    }

    /*
    * Read is for point query over all columns
    * Here Read will find the first key/value pair that the index is pointing to and return.
    */
    int Indexing::Read(const std::string &table, const std::string &key,
                        const std::set<std::string> *fields, const std::string &req_dist,
                        bool index_access, std::string &result) 
    {
        rocksdb::Status s;
        if (index_access) {
            for (int i = 1; i < 6; i++) {
                std::string valuekeysstr;
                s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table+"_index_cf"], key, &valuekeysstr);
                if (valuekeysstr != "") {
                    std::vector<std::string> valuekeys = parsePrimaryKeys(valuekeysstr);
                    for (auto vkey : valuekeys) {
                        s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table], vkey, &result);
                    }
                }
            }
        } else {
            s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table], key, &result);
        }

        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    int Indexing::Scan(const std::string &table, const std::string &begin_key,
                       const std::string &end_key, const std::set<std::string> *fields,
                       const std::string &req_dist, bool index_access,
                       std::vector<std::string> &result) 
    {
        rocksdb::Status s;
        if (index_access) {
            std::set<std::string> origkeys;
            int searched = 0;
            auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandles_[table+"_index_cf"]);
            it->Seek(begin_key);
            while (it->Valid() && searched < 25) {
                std::vector<std::string> valuekeys = parsePrimaryKeys(it->value().ToString());
                for (auto vkey : valuekeys) {
                    origkeys.insert(vkey);
                }
                    
                it->Next();
                searched++;
            }

            for (auto origkey : origkeys) {
                std::string vvalue;
                s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table], origkey, &vvalue);
                result.push_back(vvalue);
            }
        } else {
            int searched = 0;
            auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandles_[table]);
            it->Seek(begin_key);
            while (it->Valid() && searched < 25) {
                result.push_back(it->value().ToString());
                
                it->Next();
                searched++;
            }
        }

        if (result.size() > 0) {
            return 0;
        }
        return 1;
    }

    int Indexing::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        rocksdb::Status s = rocksdb_->Put(rocksdb::WriteOptions(),
                                      cfhandles_[table],
                                      key,
                                      values);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    int Indexing::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int Indexing::Delete(const std::string &table, const std::string &key)
    {
        rocksdb::Status s = rocksdb_->Delete(rocksdb::WriteOptions(),
                                             cfhandles_[table],
                                             key);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    void Indexing::SetOptions(const char *dbfilename, bool logging, int levels, int fieldcount,
                rocksdb::InputOutputDataType inputDataType, rocksdb::InputOutputDataType outputDataType)
    {
        if (!logging) {
            options_.info_log_level = rocksdb::InfoLogLevel::FATAL_LEVEL;
        }
        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;
        options_.merge_operator = std::make_shared<SecondaryIndexMergeOperator>();

        options_.num_levels = levels;
        options_.num_columns = fieldcount;
        options_.SetTransformerType(rocksdb::TransformerType::AUGMENTER);
        options_.SetInputOutputDataType(inputDataType, outputDataType);

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
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(dbname,
                                                                  rocksdb::ColumnFamilyOptions(options_)));
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(dbname+"_index_cf",
                                                                  rocksdb::ColumnFamilyOptions(options_)));
    }

    void Indexing::BuildColumnFamilyHandleMap(std::vector<rocksdb::ColumnFamilyDescriptor>& column_family_descriptors,
                                              std::vector<rocksdb::ColumnFamilyHandle*> handles)
    {
        for (size_t i = 0; i < handles.size(); i++) {
            cfhandles_.insert({column_family_descriptors[i].name, handles[i]});
        }
    }

    std::vector<std::string> Indexing::deserializeIndex(const std::string& serialized)
    {
        std::istringstream iss(serialized);
        std::vector<std::string> result;

        size_t num_strings;
        iss.read(reinterpret_cast<char*>(&num_strings), sizeof(num_strings));

        for (size_t i = 0; i < num_strings; i++) {
            size_t str_len;
            iss.read(reinterpret_cast<char*>(&str_len), sizeof(str_len));

            std::string str(str_len, '\0');
            iss.read(&str[0], str_len);

            result.push_back(str);
        }

        return result;
    }

    std::vector<std::string> Indexing::parsePrimaryKeys(const std::string& value) {
        std::vector<std::string> primary_keys;
        std::istringstream stream(value);
        std::string key;
    
        while (std::getline(stream, key, ',')) {
            primary_keys.push_back(key);
        }

        return primary_keys;
    }

    rocksdb::DeriveFuncData* Indexing::CreateIndexer(std::vector<int> positions) {
        std::function<std::string(std::vector<std::string>&)> f = [&](std::vector<std::string>& strs) -> std::string {
            if (strs.size() == 0) {
                return "";
            }
            if (strs.size() == 1) {
                return strs[0];
            }
            std::string ind = strs[0];
            size_t total_length = ind.size();
            for (size_t i = 1; i < strs.size(); ++i) {
                total_length += (1 + strs[i].size());       // need to add a delimiter ","
            }
            ind.reserve(total_length);

            for (size_t i = 1; i < strs.size(); i++) {
                ind += "," + strs[i];
            }
            return ind;
        };
        return new rocksdb::DeriveFuncData(positions, f);
    }

}
