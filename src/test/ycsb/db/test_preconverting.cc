#include <nlohmann/json.hpp>
#include "core/core_workload.h"
#include "test_preconverting.h"
#include "lib/coding.h"
#include "flatbuffers/flatbuffers.h"
#include "flat/row_generated.h"
#include "flat/row_num_generated.h"
#include "flat/row_str_generated.h"

using namespace std;

namespace ycsbc {
    TestPreconverting::TestPreconverting(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        noResults = 0;
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        int levels = utils::StrToInt(props.GetProperty("levels", "6"));
        int fieldcount = utils::StrToInt(props.GetProperty("fieldcount", "16"));
        rocksdb::InputOutputDataType inputType = ycsbc::DBHelper::mapStringToDataType(props.GetProperty("inputdatatype", "JSON"));
        rocksdb::InputOutputDataType outputType = ycsbc::DBHelper::mapStringToDataType(props.GetProperty("outputdatatype", "FLATBUFFERS"));
        SetOptions(dbfilename, levels, fieldcount, bootstrap, inputType, outputType);
        write_options_.disableWAL = true;
        std::vector<rocksdb::ColumnFamilyDescriptor> column_family_descriptors;
        GetColumnFamilyDescriptors(dbname, column_family_descriptors);
        std::vector<rocksdb::ColumnFamilyHandle*> cf_handles;

        if (bootstrap) {
            rocksdb::Status s = rocksdb::DB::Open(options_, 
                                              dbfilename,
                                              &rocksdb_);
            if (!s.ok()){
                std::cerr<<"Can't open preconverting "<<dbfilename<<" "<<s.ToString()<<std::endl;
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
                std::cerr<<"Can't open preconverting "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }
        }
        BuildColumnFamilyHandleMap(column_family_descriptors, cf_handles);
    }

    /*
    * Read is for point query over all columns
    */
    int TestPreconverting::Read(const std::string &table, const std::string &key, const std::set<std::string> *fields,
                      const std::string &req_dist, bool index_access, std::string &result) 
    {
        rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandle_, key, &result);
        if (s.ok()) {    
            return 0;
        }
        return 1;
    }

    int TestPreconverting::Scan(const std::string &table, const std::string &begin_key,
                          const std::string &end_key, const std::set<std::string> *fields,
                          const std::string &req_dist, bool index_access,
                          std::vector<std::string> &result) 
    {
        result.clear();
        auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandle_);
        int searched = 0;
        it->Seek(begin_key);
        while (it->Valid() && searched < 25) {
            result.push_back(it->value().ToString());
            it->Next();
            searched++;
        }
        return result.size();
    }

    int TestPreconverting::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        nlohmann::json parsedJson = nlohmann::json::parse(values);

        flatbuffers::FlatBufferBuilder builder;
        std::vector<int32_t> numvals;
        std::vector<flatbuffers::Offset<flatbuffers::String>> strvals;

        for (const auto& element : parsedJson) {
            if (element.is_number()) {
                numvals.push_back(element.get<int>());
            } else if (element.is_string()) {
                strvals.push_back(builder.CreateString(element.get<std::string>()));
            }
        }

        auto num_vector = builder.CreateVector(numvals);
        auto col_vector = builder.CreateVector(strvals);
        auto fb_row = rocksdb::CreateFbRow(builder, num_vector, col_vector);
        builder.Finish(fb_row);
            
        uint8_t *buf = builder.GetBufferPointer();
        int size = builder.GetSize();
        std::string str(reinterpret_cast<char*>(buf), size);

        rocksdb::Status s = rocksdb_->Put(write_options_,
                                  cfhandle_,
                                  key,
                                  str);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    int TestPreconverting::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int TestPreconverting::Delete(const std::string &table, const std::string &key)
    {
        rocksdb::Status s = rocksdb_->Delete(write_options_, cfhandle_, key);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    void TestPreconverting::SetOptions(const char *dbfilename, int levels, int fieldcount, bool logging,
                rocksdb::InputOutputDataType inputDataType, rocksdb::InputOutputDataType outputDataType)
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
        options_.SetInputOutputDataType(inputDataType, outputDataType);

        options_.write_buffer_size = 128 * 1024 * 1024;
        options_.max_write_buffer_number = 8;
        options_.level0_file_num_compaction_trigger = 4;
        options_.level0_slowdown_writes_trigger = 20;
        options_.level0_stop_writes_trigger = 32;
        options_.IncreaseParallelism(24);
        options_.use_direct_reads = true;
        options_.use_direct_io_for_flush_and_compaction = true;
        
        options_.target_file_size_base = 256 * 1024 * 1024;
        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_cache = rocksdb::NewLRUCache(256 * 1024 * 1024);
        options_.table_factory = std::shared_ptr<rocksdb::TableFactory>(rocksdb::NewBlockBasedTableFactory(table_options));
    }

    void TestPreconverting::GetColumnFamilyDescriptors(const std::string& dbname,
                    std::vector<rocksdb::ColumnFamilyDescriptor>& column_families)
    {
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(
                dbname+"_converted_cf", rocksdb::ColumnFamilyOptions(options_)));
        
    }

    void TestPreconverting::BuildColumnFamilyHandleMap(std::vector<rocksdb::ColumnFamilyDescriptor>& column_family_descriptors,
                            std::vector<rocksdb::ColumnFamilyHandle*> handles)
    {
        for (size_t i = 0; i < handles.size(); i++) {
            if (column_family_descriptors[i].name != rocksdb::kDefaultColumnFamilyName) {
                cfhandle_ = handles[i];
            }
        }
    }

    std::set<int> TestPreconverting::GetQueryingHandles(std::set<std::string> fields) {
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