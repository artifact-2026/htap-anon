#include "core/core_workload.h"
#include "test_rocks_db.h"
#include "lib/coding.h"
#include "test_flatbuffers.h"
#include "transformer/converter.h"

#include <iostream>
#include <iomanip> // Include for std::setfill and std::setw

using namespace std;

namespace ycsbc {
    TestFlatBuffers::TestFlatBuffers(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        int levels = utils::StrToInt(props.GetProperty("levels", "6"));
        int fieldcount = utils::StrToInt(props.GetProperty("fieldcount", "1"));
        noResults = 0;
        SetOptions(props, bootstrap, levels, fieldcount);

        options_.transformers.push_back(new rocksdb::Converter());

        std::vector<rocksdb::ColumnFamilyDescriptor> column_family_descriptors;
        GetColumnFamilyDescriptors(dbname, column_family_descriptors);
        std::vector<rocksdb::ColumnFamilyHandle*> cf_handles;

        if (bootstrap) {
            rocksdb::Status s = rocksdb::DB::Open(options_, 
                                              dbfilename,
                                              &rocksdb_);
            if (!s.ok()){
                std::cerr<<"Can't open flatbuffers "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }

            s = rocksdb_->CreateColumnFamilies(column_family_descriptors, &cf_handles);
            s = rocksdb_->AddTransformingDestinationCfds(dbname, false, true, false);
            if (!s.ok()){
                std::cerr<<"Creating column families for flatbuffers db ran into error "<<s.ToString()<<std::endl;
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
                std::cerr<<"Can't open flatbuffers "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }

            s = rocksdb_->AddTransformingDestinationCfds(dbname, false, true, false);
            if (!s.ok()){
                std::cerr<<"Creating column families for flatbuffers db ran into error "<<s.ToString()<<std::endl;
                exit(0);
            }
        }
        BuildColumnFamilyHandles(column_family_descriptors, cf_handles);
        rocksdb_->DisplayTransformingDestinationCfds();
    }
        
    void TestFlatBuffers::GetColumnFamilyDescriptors(const std::string& dbname,
                    std::vector<rocksdb::ColumnFamilyDescriptor>& column_families)
    {
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(
            dbname, rocksdb::ColumnFamilyOptions(options_)));
        options_.num_levels -= 1;
        options_.SetTransformerType(rocksdb::TransformerType::NOTRANSFORMATION);
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(
            dbname+"_converted_cf", rocksdb::ColumnFamilyOptions(options_)));
    }

    /*
    * Read is for point query over all columns
    */
    int TestFlatBuffers::Read(const std::string &table, const std::string &key, const std::set<std::string> *fields,
                      const std::string &req_dist, bool index_access, std::string &result) 
    {
        rocksdb::Status s;

        if (req_dist == "earliest") {
            s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table+"_converted_cf"], key, &result);
            if (result != "") {
                return 0;
            }
        } else {
            s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table], key, &result);
            if (result != "") {
                return 0;
            }
            s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table+"_converted_cf"], key, &result);
            if (result != "") {
                return 0;
            }
        }

        return 1;
    }

    int TestFlatBuffers::Scan(const std::string &table, const std::string &begin_key,
                          const std::string &end_key, const std::set<std::string> *fields,
                          const std::string &req_dist, bool index_access,
                          std::vector<std::string> &result) 
    {
        if (req_dist == "ealiest") {
            auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandles_[table+"_converted_cf"]);
            it->Seek(begin_key);
            while (it->Valid()) {
                if (it->key().ToString() < end_key) {
                    result.push_back(it->value().ToString());
                } else {
                    break;
                }
                it->Next();
            }
        } else {
            std::set<std::string> keyset;
            auto itt = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandles_[table]);
            itt->Seek(begin_key);
            while (itt->Valid()) {
                if (itt->key().ToString() < end_key) {
                    result.push_back(itt->value().ToString());
                    keyset.insert(itt->key().ToString());
                } else {
                    break;
                }
                itt->Next();
            }

            auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandles_[table+"_converted_cf"]);
            it->Seek(begin_key);
            while (it->Valid()) {
                if (it->key().ToString() < end_key) {
                    if (keyset.find(it->key().ToString()) != keyset.end()) {
                        result.push_back(it->value().ToString());
                    }
                } else {
                    break;
                }
                it->Next();
            }
        }
        
        if (result.size() > 0) {
            return 0;
        }
        return 1;
    }

    int TestFlatBuffers::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        rocksdb::Status s = rocksdb_->Put(rocksdb::WriteOptions(), cfhandles_[table], key, values);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    int TestFlatBuffers::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int TestFlatBuffers::Delete(const std::string &table, const std::string &key)
    {
        rocksdb::Status s = rocksdb_->Delete(rocksdb::WriteOptions(), cfhandles_[table], key);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    void TestFlatBuffers::SetOptions(utils::Properties &props, bool logging, int levels, int fieldcount)
    {
        if (!logging) {
            options_.info_log_level = rocksdb::InfoLogLevel::FATAL_LEVEL;
        }
        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;

        options_.num_levels = levels;
        options_.num_columns = fieldcount;
        options_.SetTransformerType(rocksdb::TransformerType::CONVERTER);

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

        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_cache = nullptr;  // Disable the block cache
        options_.table_factory = std::shared_ptr<rocksdb::TableFactory>(rocksdb::NewBlockBasedTableFactory(table_options));
    }

    void TestFlatBuffers::KeepOnlyRequestedFields(data::Row &row,
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

    void TestFlatBuffers::BuildColumnFamilyHandles(std::vector<rocksdb::ColumnFamilyDescriptor> &column_family_descriptors,
                                                std::vector<rocksdb::ColumnFamilyHandle *> handles)
    {
        for (size_t i = 0; i < handles.size(); i++) {
            if (column_family_descriptors[i].name != rocksdb::kDefaultColumnFamilyName) {
                cfhandles_.insert({column_family_descriptors[i].name, handles[i]});
            }
        }
    }

}
