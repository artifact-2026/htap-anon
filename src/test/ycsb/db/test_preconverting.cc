#include "core/core_workload.h"
#include "test_preconverting.h"
#include "lib/coding.h"
#include "flatbuffers/flatbuffers.h"
#include "flat/data_generated.h"

using namespace std;

namespace ycsbc {
    TestPreconverting::TestPreconverting(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        noResults = 0;
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        int levels = utils::StrToInt(props.GetProperty("levels", "6"));
        int fieldcount = utils::StrToInt(props.GetProperty("fieldcount", "16"));
        SetOptions(dbfilename, levels, fieldcount, bootstrap);

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
        it->Seek(begin_key);
        while (it->Valid()) {
            if (it->key().ToString() < end_key) {
                result.push_back(it->value().ToString());
            } else {
                break;
            }
            it->Next();
        }
        return result.size();
    }

    int TestPreconverting::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        data::Row row;
        row.ParseFromString(values);

        flatbuffers::FlatBufferBuilder builder;

        // Add the columns as uint64s
        std::vector<flatbuffers::Offset<NumericColumn>> numericCols;
        for (int i = 0; i < row.columns_size(); ++i) {
            try {
                const std::string& col_name_str = row.columns(i).name();
                const std::string& value_str = row.columns(i).value();
                auto col_name = builder.CreateString(col_name_str);
                auto col = CreateNumericColumn(builder, col_name, std::stoull(value_str));
                numericCols.push_back(col);
            } catch (const std::invalid_argument& ia) {
                std::cerr << "Catching invalid argument exception: " << ia.what() << std::endl;
                continue;
            } catch (const std::out_of_range& orr) {
                std::cerr << "Catching out of range exception: " << orr.what() << std::endl;
                continue;
            }
        }

        // Add vectors to the builder
        auto numericVec = builder.CreateVector(numericCols);

        // Create the FbRow object
        auto fbRow = CreateFbRow(builder, numericVec);

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

    void TestPreconverting::SetOptions(const char *dbfilename, int levels, int fieldcount, bool logging)
    {
        if (!logging) {
            options_.info_log_level = rocksdb::InfoLogLevel::FATAL_LEVEL;
        }
        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;

        options_.num_levels = levels;
        options_.num_columns = fieldcount;
        options_.SetTransformerType(rocksdb::TransformerType::NOTRANSFORMATION);

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
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(dbname+"_converted_cf",
                                                                  rocksdb::ColumnFamilyOptions(options_)));
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