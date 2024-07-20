#include "core/core_workload.h"
#include "test_precracking.h"
#include "lib/coding.h"
#include "cabindb/rocksdb-rados-env/env_librados.h"

using namespace std;

namespace ycsbc {
    RocksdbColumnStrawman::RocksdbColumnStrawman(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        noResults = 0;
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        int num_cfs = stoi(props.GetProperty("fieldcount", "0"));
        SetOptions(props, dbfilename, num_cfs, bootstrap);

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
                      std::string &result) 
    {
        if (fields == nullptr) {
            for (auto handle : handleList_) {
                std::string value;
                rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(),
                                              handle,
                                              key,
                                              &value);
        
                if (value == "") {
                    noResults++;
                    return 1;
                }

                result += value;
            }
            return 0;
        }

        std::set<int> queryCols;
        queryCols = GetQueryingHandles(std::set<std::string>(fields->begin(), fields->end()));

        for (auto qc : queryCols) {
            std::string value;
            rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(),
                                          handleList_[qc],
                                          key,
                                          &value);
        
            if (!s.ok()) {
                noResults++;
                return 1;
            }
        }
        return 0;
    }

    int RocksdbColumnStrawman::Scan(const std::string &table, const std::string &begin_key,
                          int32_t len, const std::set<std::string> *fields,
                          std::vector<std::string> &result) 
    {
        std::set<int> queryCols;
        if (fields != nullptr) {
            queryCols = GetQueryingHandles(std::set<std::string>(fields->begin(), fields->end()));
        }

        int queryPos = 0;
        if (queryCols.size() > 0) {
            auto itt = queryCols.begin();
            queryPos = *itt;
        }

        auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), handleList_[queryPos]);
        it->Seek(begin_key);
        for (int i = 0; i < len && it->Valid(); i++) {
            std::string val = it->value().ToString();
            result.push_back(val);
            it->Next();
        }

        return result.size();
    }

    int RocksdbColumnStrawman::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        data::Row row;
        row.ParseFromString(values);
        for (int i = 0; i < row.columns_size()-1; i++) {
            std::string serializedColumn1, serializedColumn2;
            row.columns(i).SerializeToString(&serializedColumn1);
            row.columns(++i).SerializeToString(&serializedColumn2);
            rocksdb::Status s = rocksdb_->Put(rocksdb::WriteOptions(),
                                          cfhandles_[table+"_col_"+std::to_string(i/2)],
                                          key,
                                          serializedColumn1+serializedColumn2);
            if (!s.ok()) {
                return 1;
            }
        }
    
        return 0;
    }

    int RocksdbColumnStrawman::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int RocksdbColumnStrawman::Delete(const std::string &table, const std::string &key)
    {
        rocksdb::Status s = rocksdb_->Delete(rocksdb::WriteOptions(),
                                             cfhandles_[rocksdb::kDefaultColumnFamilyName],
                                             key);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    void RocksdbColumnStrawman::SetOptions(utils::Properties &props, const char *dbfilename, int num_cfs, bool logging)
    {
        if (!logging) {
            options_.info_log_level = rocksdb::InfoLogLevel::FATAL_LEVEL;
        }
        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;
        options_.num_columns = num_cfs;

        options_.IncreaseParallelism(16);
        options_.level0_slowdown_writes_trigger = 16;     
        options_.level0_stop_writes_trigger = 24;
        options_.max_open_files = -1;
        options_.level0_file_num_compaction_trigger = 8;

        options_.max_write_buffer_number = 3;
        options_.write_buffer_size = 67108864;
        options_.target_file_size_base = 67108864;

        options_.num_levels = 4;

        //options_.max_background_jobs = 16;
        //options_.db_write_buffer_size = 2 << 30;

        options_.use_direct_reads = true;
        options_.use_direct_io_for_flush_and_compaction = true;

        //options_.max_open_files = 20480;
        //options_.max_file_opening_threads = 32;
    }

    void RocksdbColumnStrawman::KeepOnlyRequestedFields(data::Row &row,
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

    void RocksdbColumnStrawman::GetColumnFamilyDescriptors(const std::string& dbname,
                    std::vector<rocksdb::ColumnFamilyDescriptor>& column_families)
    {
        int splits = 1;
        for (int i = 0; i < options_.num_levels-1; i++) {
            splits *= 2;
        }

        for (int i = 0; i < splits; i++) {
            std::string cf_name = dbname + "_col_" + std::to_string(i);
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
