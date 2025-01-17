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
        rocksdb::InputOutputDataType inputType = ycsbc::DBHelper::mapStringToDataType(props.GetProperty("inputdataformat", "PROTOBUF"));
        rocksdb::InputOutputDataType outputType = ycsbc::DBHelper::mapStringToDataType(props.GetProperty("outputdataformat", "FLATBUFFERS"));
        std::string columnDataType = props.GetProperty("columndatatype", "1");

        SetOptions(props, false, levels, fieldcount, inputType, outputType, columnDataType);
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
            s = rocksdb_->AddTransformingDestinationCfds(dbname, false, true, false, false, 0);
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

            s = rocksdb_->AddTransformingDestinationCfds(dbname, false, true, false, false, 0);
            if (!s.ok()){
                std::cerr<<"Creating column families for flatbuffers db ran into error "<<s.ToString()<<std::endl;
                exit(0);
            }
        }
        BuildColumnFamilyHandles(column_family_descriptors, cf_handles);
    }
        
    void TestFlatBuffers::GetColumnFamilyDescriptors(const std::string& dbname,
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
            dbname+"_converted_cf", rocksdb::ColumnFamilyOptions(options_)));
    }

    /*
    * Read is for point query over all columns
    */
    int TestFlatBuffers::Read(const std::string &table, const std::string &key, const std::set<std::string> *fields,
                      const std::string &req_dist, bool index_access, std::string &result) 
    {
        rocksdb::Status s;

        s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table], key, &result);
        if (s.ok()) {
            return 0;
        }
        s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table+"_converted_cf"], key, &result);
        if (s.ok()) {
            return 0;
        }

        return 1;
    }

    int TestFlatBuffers::Scan(const std::string &table, const std::string &begin_key,
                          const std::string &end_key, const std::set<std::string> *fields,
                          const std::string &req_dist, bool index_access,
                          std::vector<std::string> &result) 
    {
        uint64_t searched = 100;
        uint64_t sum = 0;
        
        auto itt = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandles_[table]);
        itt->Seek(begin_key);
        while (itt->Valid() && result.size() < searched) {
            if (fields != nullptr) {
                flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(itt->value().data()), itt->value().size());
                if (rocksdb::VerifyFbRowBuffer(verifier)) {
                    const uint8_t* buf = reinterpret_cast<const uint8_t*>(itt->value().data());
                    auto fb_row = rocksdb::GetFbRow(buf);
                    if (fb_row && fb_row->numcols() && fb_row->numcols()->size() > 0) {
                        sum += fb_row->numcols()->Get(0);
                    }
                }
            }
            result.push_back(itt->value().ToString());
            itt->Next();
        }
        if (result.size() >= 100) {
            return 0;
        } else {
            searched -= result.size();
        }

        auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandles_[table+"_converted_cf"]);
        it->Seek(begin_key);
        while (it->Valid() && result.size() < searched) {
            if (fields != nullptr) {
                flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(itt->value().data()), itt->value().size());
                if (rocksdb::VerifyFbRowBuffer(verifier)) {
                    const uint8_t* buf = reinterpret_cast<const uint8_t*>(itt->value().data());
                    auto fb_row = rocksdb::GetFbRow(buf);
                    if (fb_row && fb_row->numcols() && fb_row->numcols()->size() > 0) {
                        sum += fb_row->numcols()->Get(0);
                    }
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

    int TestFlatBuffers::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        rocksdb::Status s = rocksdb_->Put(write_options_, cfhandles_[table], key, values);
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
        rocksdb::Status s = rocksdb_->Delete(write_options_, cfhandles_[table], key);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    void TestFlatBuffers::SetOptions(utils::Properties &props, bool logging, int levels, int fieldcount,
        rocksdb::InputOutputDataType inputDataType, rocksdb::InputOutputDataType outputDataType, std::string columndatatype)
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
        options_.column_data_type = columndatatype;
        options_.SetTransformerType(rocksdb::TransformerType::CONVERTER);
        options_.SetInputOutputDataType(inputDataType, outputDataType);

        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_cache = rocksdb::NewLRUCache(512 * 1024 * 1024);
        table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
        options_.table_factory = std::shared_ptr<rocksdb::TableFactory>(rocksdb::NewBlockBasedTableFactory(table_options));
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
