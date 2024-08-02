#include "core/core_workload.h"
#include "test_rocks_db.h"
#include "lib/coding.h"
#include "test_flatbuffers.h"
#include "lib/rocksdb/transformer/converter.h"

#include <iostream>
#include <iomanip> // Include for std::setfill and std::setw

using namespace std;

namespace ycsbc {
    TestFlatBuffers::TestFlatBuffers(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        int levels = utils::StrToInt(props.GetProperty("levels", "7"));
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
        }
        BuildColumnFamilyHandles(column_family_descriptors, cf_handles);
    }
        
    void TestFlatBuffers::GetColumnFamilyDescriptors(const std::string& dbname,
                    std::vector<rocksdb::ColumnFamilyDescriptor>& column_families)
    {
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(
                "flatbuffers", rocksdb::ColumnFamilyOptions(options_)));
    }

    /*
    * Read is for point query over all columns
    */
    int TestFlatBuffers::Read(const std::string &table, const std::string &key, const std::set<std::string> *fields,
                      std::string &result) 
    {
        std::string value;
        rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandle_, key, &value);
        size_t fieldsFound = 0;

        if (s.ok()) {
            std::vector<uint8_t> vec(value.begin(), value.end());
            flatbuffers::Verifier verifier(vec.data(), vec.size());
            const FbRow* fbRow = GetFbRow(vec.data());

            if (fbRow != nullptr && fbRow->Verify(verifier)) {
                const flatbuffers::Vector<flatbuffers::Offset<NumericColumn>>* numcols = fbRow->numcols();
                if (numcols != nullptr) {
                    for (size_t i = 0; i < numcols->size(); i++) {
                        if (fields == nullptr || fields->find(numcols->Get(i)->name()->str()) != fields->end()) {
                            result += numcols->Get(i)->name()->str() + "::" + std::to_string(numcols->Get(i)->value());
                            fieldsFound++;
                            if (fields != nullptr && fieldsFound >= fields->size()) {
                                break;
                            }
                        }
                    }    
                }
            }
            return 0;
        }
    
        noResults++;
        return 1;
    }

    int TestFlatBuffers::Scan(const std::string &table, const std::string &begin_key,
                          int32_t len, const std::set<std::string> *fields,
                          std::vector<std::string> &result) 
    {
        auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandle_);
        it->Seek(begin_key);
        for (int i = 0; i < len && it->Valid(); i++) {
            std::string value = it->value().ToString();
            std::vector<uint8_t> vec(value.begin(), value.end());
            flatbuffers::Verifier verifier(vec.data(), vec.size());

            const FbRow* fbRow = GetFbRow(vec.data());
            std::string rowResult;
            size_t fieldsFound = 0;
            
            if (fbRow != nullptr && fbRow->Verify(verifier)) {
                const flatbuffers::Vector<flatbuffers::Offset<NumericColumn>>* numcols = fbRow->numcols();
                if (numcols != nullptr) {
                    for (size_t i = 0; i < numcols->size(); i++) {
                        if (fields == nullptr || fields->find(numcols->Get(i)->name()->str()) != fields->end()) {
                            rowResult += numcols->Get(i)->name()->str() + "::" + std::to_string(numcols->Get(i)->value());
                            fieldsFound++;
                            if (fields != nullptr && fieldsFound >= fields->size()) {
                                break;
                            }
                        }
                    }    
                }
            }
            result.push_back(rowResult);
            it->Next();
        }
        return result.size();
    }

    int TestFlatBuffers::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        rocksdb::Status s = rocksdb_->Put(rocksdb::WriteOptions(), cfhandle_, key, values);
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
        rocksdb::Status s = rocksdb_->Delete(rocksdb::WriteOptions(), cfhandle_, key);
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
                std::cout << "column family handle: " << column_family_descriptors[i].name << std::endl;
                cfhandle_ = handles[i];
            }
        }
    }

}
