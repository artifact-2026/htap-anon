#include "core/core_workload.h"
#include "test_preindexing.h"
#include "lib/coding.h"

using namespace std;

namespace ycsbc {
    TestPreindexing::TestPreindexing(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
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
                std::cerr<<"Can't open preindexing "<<dbfilename<<" "<<s.ToString()<<std::endl;
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
                std::cerr<<"Can't open preindexing "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }
        }
        BuildColumnFamilyHandleMap(column_family_descriptors, cf_handles);
    }

    /*
    * Read is for point query over all columns
    */
    int TestPreindexing::Read(const std::string &table, const std::string &key, const std::set<std::string> *fields,
                      std::string &result) 
    {
        std::string value;
        rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[0], key, &value);
        
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    int TestPreindexing::Scan(const std::string &table, const std::string &begin_key,
                          int32_t len, const std::set<std::string> *fields,
                          std::vector<std::string> &result) 
    {
        result.clear();
        auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandles_[0]);
        it->Seek(begin_key);

        for (int i = 0; i < len && it->Valid(); i++) {
            result.push_back(it->value().ToString());
            it->Next();
        }
        return result.size();
    }

    int TestPreindexing::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        data::Row row;
        row.ParseFromString(values);
        const std::string ikey = row.columns(2).value();

        std::string indvalues;
        rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[1], ikey, &indvalues);

        if (indvalues == "") {
            indvalues += key;
        } else {
            indvalues += "%%";
            indvalues += key;
        }
        s = rocksdb_->Put(rocksdb::WriteOptions(), cfhandles_[1], ikey, indvalues);
                
        if (s.ok()) {
            s = rocksdb_->Put(rocksdb::WriteOptions(), cfhandles_[0], key, values);
            if (s.ok()) {
                return 0;
            }
        }
        return 1;
    }

    int TestPreindexing::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int TestPreindexing::Delete(const std::string &table, const std::string &key)
    {
        std::string values;
        rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[0], key, &values);

        data::Row row;
        row.ParseFromString(values);
        const std::string ikey = row.columns(2).value();

        std::string indvalues;
        s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[1], ikey, &indvalues);

        size_t pos = 0, start = 0;
        std::string newindvalues;
        while ((pos = indvalues.find("%%", start)) != std::string::npos) {
            std::string token = indvalues.substr(start, pos - start);
            if (token != key) {
                newindvalues += token;
                start = pos + 2;
            } else {
                newindvalues += indvalues.substr(pos+2);
                break;
            }
        }

        if (pos == std::string::npos) {
            if (indvalues.substr(start) != key) {
                newindvalues += indvalues.substr(start);
            }
        }

        s = rocksdb_->Put(rocksdb::WriteOptions(), cfhandles_[1], ikey, newindvalues);

        s = rocksdb_->Delete(rocksdb::WriteOptions(), cfhandles_[0], key);

        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    void TestPreindexing::SetOptions(const char *dbfilename, int levels, int fieldcount, bool logging)
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

    void TestPreindexing::KeepOnlyRequestedFields(data::Row &row,
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

    void TestPreindexing::GetColumnFamilyDescriptors(const std::string& dbname,
                    std::vector<rocksdb::ColumnFamilyDescriptor>& column_families)
    {
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(dbname,
                                                                  rocksdb::ColumnFamilyOptions(options_)));
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(dbname+"_derived_cf_0",
                                                                  rocksdb::ColumnFamilyOptions(options_)));
    }

    void TestPreindexing::BuildColumnFamilyHandleMap(std::vector<rocksdb::ColumnFamilyDescriptor>& column_family_descriptors,
                            std::vector<rocksdb::ColumnFamilyHandle*> handles)
    {
        for (size_t i = 0; i < handles.size(); i++) {
            if (column_family_descriptors[i].name != rocksdb::kDefaultColumnFamilyName) {
                cfhandles_.push_back(handles[i]);
            }
        }
    }

    std::set<int> TestPreindexing::GetQueryingHandles(std::set<std::string> fields) {
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
