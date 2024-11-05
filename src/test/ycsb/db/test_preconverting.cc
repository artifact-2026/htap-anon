#include <nlohmann/json.hpp>
#include "core/core_workload.h"
#include "test_preconverting.h"
#include "lib/coding.h"
#include "flatbuffers/flatbuffers.h"
#include "flat/row_generated.h"
#include "flat/column_num_generated.h"
#include "flat/column_str_generated.h"

using namespace std;

namespace ycsbc {
    TestPreconverting::TestPreconverting(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        noResults = 0;
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        int levels = utils::StrToInt(props.GetProperty("levels", "6"));
        int fieldcount = utils::StrToInt(props.GetProperty("fieldcount", "16"));
        rocksdb::InputOutputDataType inputType = ycsbc::DBHelper::mapStringToDataType(props.GetProperty("inputdatatype", "PROTOBUF"));
        rocksdb::InputOutputDataType outputType = ycsbc::DBHelper::mapStringToDataType(props.GetProperty("outputdatatype", "FLATBUFFERS"));
        SetOptions(dbfilename, levels, fieldcount, bootstrap, inputType, outputType);

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
        data::Row row;
        row.ParseFromString(values);
        //nlohmann::json parsedJson = nlohmann::json::parse(values);

        flatbuffers::FlatBufferBuilder builder;

        /*auto field8 = builder.CreateString(parsedJson["field8"].get<std::string>());
        auto field9 = builder.CreateString(parsedJson["field9"].get<std::string>());
        auto field10 = builder.CreateString(parsedJson["field10"].get<std::string>());
        auto field11 = builder.CreateString(parsedJson["field11"].get<std::string>());
        auto field12 = builder.CreateString(parsedJson["field12"].get<std::string>());
        auto field13 = builder.CreateString(parsedJson["field13"].get<std::string>());
        auto field14 = builder.CreateString(parsedJson["field14"].get<std::string>());
        auto field15 = builder.CreateString(parsedJson["field15"].get<std::string>());

        auto fbRow = rocksdb::CreateFbRow(
            builder,
            parsedJson["field0"].get<int>(),
            parsedJson["field1"].get<int>(),
            parsedJson["field2"].get<int>(),
            parsedJson["field3"].get<int>(),
            parsedJson["field4"].get<int>(),
            parsedJson["field5"].get<int>(),
            parsedJson["field6"].get<int>(),
            parsedJson["field7"].get<int>(),
            field8, field9, field10, field11, field12, field13, field14, field15
        );*/

        auto field8 = builder.CreateString(row.columns(8).value());
        auto field9 = builder.CreateString(row.columns(9).value());
        auto field10 = builder.CreateString(row.columns(10).value());
        auto field11 = builder.CreateString(row.columns(11).value());
        auto field12 = builder.CreateString(row.columns(12).value());
        auto field13 = builder.CreateString(row.columns(13).value());
        auto field14 = builder.CreateString(row.columns(14).value());
        auto field15 = builder.CreateString(row.columns(15).value());

        auto fbRow = rocksdb::CreateFbRow(
            builder,
            stoi(row.columns(0).value()),
            stoi(row.columns(1).value()),
            stoi(row.columns(2).value()),
            stoi(row.columns(3).value()),
            stoi(row.columns(4).value()),
            stoi(row.columns(5).value()),
            stoi(row.columns(6).value()),
            stoi(row.columns(7).value()),
            field8, field9, field10, field11, field12, field13, field14, field15
        );

        builder.Finish(fbRow);

        uint8_t *buf = builder.GetBufferPointer();
        int size = builder.GetSize();
        std::string str(reinterpret_cast<char*>(buf), size);

        rocksdb::Status s = rocksdb_->Put(rocksdb::WriteOptions(),
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
        rocksdb::Status s = rocksdb_->Delete(rocksdb::WriteOptions(), cfhandle_, key);
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

        options_.num_levels = levels;
        options_.num_columns = fieldcount;
        options_.SetTransformerType(rocksdb::TransformerType::NOTRANSFORMATION);
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

    void TestPreconverting::KeepOnlyRequestedFields(data::Row &row,
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