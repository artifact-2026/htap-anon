#include "core/core_workload.h"
#include "rocksdb_column_strawman.h"
#include "lib/coding.h"
#include "cabindb/rocksdb-rados-env/env_librados.h"

using namespace std;

namespace ycsbc {
    RocksdbColumnStrawman::RocksdbColumnStrawman(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        noResults = 0;
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","true"));
        int num_cfs = stoi(props.GetProperty("fieldcount", "0"));
        SetOptions(props, dbfilename, num_cfs);

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
        for (auto field : *fields) {
            std::string value;
            rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(),
                                          cfhandles_[field],
                                          key,
                                          &value);
        
            if (!s.ok()) {
                noResults++;
                return 1;
            }
            result += value;
        }

        return 0;
    }

    int RocksdbColumnStrawman::Scan(const std::string &table, const std::string &begin_key,
                          int32_t len, const std::set<std::string> *fields,
                          std::vector<std::string> &result) 
    {
        result.clear();

        for (int32_t i = 0; i < len; i++) {
            std::string value = "";
            result.push_back(value);
        }

        for (auto field : *fields) {
            auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandles_[field]);
            it->Seek(begin_key);
            for (int32_t i = 0; i < len && it->Valid(); i++) {
                std::string val = it->value().ToString();
                result[i] += val;
                it->Next();
            }
        }
        
        return result.size();
    }

    int RocksdbColumnStrawman::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        data::Row row;
        row.ParseFromString(values);
        for (int i = 0; i < row.columns_size(); i++) {
            std::string serializedColumn;
            row.columns(i).SerializeToString(&serializedColumn);
            rocksdb::Status s = rocksdb_->Put(rocksdb::WriteOptions(),
                                          cfhandles_[table+"_col_"+std::to_string(i)],
                                          key,
                                          serializedColumn);
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

    void RocksdbColumnStrawman::SetOptions(utils::Properties &props, const char *dbfilename, int num_cfs)
    {
        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;
        options_.num_columns = num_cfs;

        options_.IncreaseParallelism(16);
        options_.level0_slowdown_writes_trigger = 9999999;     
        options_.level0_stop_writes_trigger = 99999999;
        options_.max_open_files = -1;
        options_.level0_file_num_compaction_trigger = 4;

        //std::string config_path = "/etc/ceph/ceph.conf";
        //std::string rados_pool;
        //rados_pool.append(dbfilename).append("_pool");
        //options_.env = new rocksdb::EnvLibrados(dbfilename, config_path, rados_pool);

        //options_.max_background_jobs = 16;
        //options_.max_write_buffer_number = 32;
        //options_.target_file_size_base = 64ul * 1024 * 1024;
        //options_.write_buffer_size = 2 << 30;
        //options_.db_write_buffer_size = 2 << 30;

        //options_.use_direct_reads = true;
        //options_.use_direct_io_for_flush_and_compaction = true;

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
        for (int i = 0; i < options_.num_columns; i++) {
            std::string cf_name = dbname + "_col_" + std::to_string(i);
            column_families.push_back(rocksdb::ColumnFamilyDescriptor(cf_name, rocksdb::ColumnFamilyOptions(options_)));
        }
    }

    void RocksdbColumnStrawman::BuildColumnFamilyHandleMap(std::vector<rocksdb::ColumnFamilyDescriptor>& column_family_descriptors,
                            std::vector<rocksdb::ColumnFamilyHandle*> handles)
    {
        for (size_t i = 0; i < handles.size(); i++) {
            cfhandles_.insert({column_family_descriptors[i].name, handles[i]});
        }
    }

}
