#include "core/core_workload.h"
#include "mycelium.h"
#include "lib/coding.h"
#include "cabindb/rocksdb-rados-env/env_librados.h"
#include "cabindb/compactor.h"

using namespace std;

namespace ycsbc {
    Mycelium::Mycelium(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        noResults = 0;
        SetOptions(props, dbfilename);
        rocksdb::CabinCompactor* compactor = new rocksdb::CabinCompactor(options_);
        options_.listeners.emplace_back(compactor);
        
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","true"));
        std::vector<rocksdb::ColumnFamilyDescriptor> column_family_descriptors;
        GetColumnFamilyDescriptors(dbname, column_family_descriptors);
        std::vector<rocksdb::ColumnFamilyHandle*> cf_handles;

        if (bootstrap) {
            rocksdb::Status s = rocksdb::DB::Open(options_, 
                                              dbfilename,
                                              &rocksdb_);
            if (!s.ok()){
                std::cerr<<"Can't open mycelium "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }

            s = rocksdb_->CreateColumnFamilies(column_family_descriptors, &cf_handles);
        } else {
            rocksdb::Status s = rocksdb::DB::Open(options_,
                                              dbfilename,
                                              column_family_descriptors,
                                              &cf_handles,
                                              &rocksdb_);
            if (!s.ok()){
                std::cerr<<"Can't open mycelium "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }
        }
        BuildColumnFamilyHandleMap(column_family_descriptors, cf_handles);
        compactor->SetColumnFamilyHandles(cfhandles_);
    }

    /*
    * Read is for point query over all columns
    */
    int Mycelium::Read(const std::string &table, const std::string &key, const std::vector<std::string> *fields,
                      data::Row &result) 
    {
        std::string value;
        auto it = cfhandles_.find(table);
        if (it != cfhandles_.end()) {
            rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(),
                                          cfhandles_[0],
                                          key,
                                          &value);
        
            if (s.ok()) {
                result.ParseFromString(value);
                return 0;
            }
        }

        noResults++;
        return 1;
    }

    int Mycelium::Scan(const std::string &table, const std::string &begin_key,
                          int32_t len, const std::vector<std::string> *fields,
                          std::vector<data::Row> &result) 
    {
        result.clear();
        auto ith = cfhandles_.find(table);
        if (ith != cfhandles_.end()) {
            auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), ith->second);
            it->Seek(begin_key);
            for (int i = 0; i < len && it->Valid(); i++) {
                std::string value = it->value().ToString();
                data::Row row;
                row.ParseFromString(value);

	            if (fields != NULL) {
                    data::Row selectedColumns;
                    KeepOnlyRequestedFields(row, fields, selectedColumns);
                    result.push_back(selectedColumns);
	            } else {
	                result.push_back(row);
                }	      
                it->Next();
            }
        }
        
        return result.size();
    }

    int Mycelium::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        auto it = cfhandles_.find(table);
        if (it != cfhandles_.end()) {
            rocksdb::Status s = rocksdb_->Put(rocksdb::WriteOptions(),
                                          it->second,
                                          key,
                                          values);
            if (s.ok()) {
                return 0;
            }
        }
        return 1;
    }

    int Mycelium::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int Mycelium::Delete(const std::string &table, const std::string &key)
    {
        auto it = cfhandles_.find(table);
        if (it != cfhandles_.end()) {
            rocksdb::Status s = rocksdb_->Delete(rocksdb::WriteOptions(),
                                             it->second,
                                             key);
            if (s.ok()) {
                return 0;
            }
        }
        return 1;
    }

    void Mycelium::SetOptions(utils::Properties &props, const char *dbfilename)
    {
        options_.create_if_missing = true;
        //options_.enable_pipelined_write = true;

        /*
        std::string config_path = "/etc/ceph/ceph.conf";
        std::string rados_pool;
        rados_pool.append(dbfilename).append("_pool");
        options_.env = new rocksdb::EnvLibrados(dbfilename, config_path, rados_pool);
        */

        //options_.max_background_jobs = 16;
        //options_.max_write_buffer_number = 32;
        options_.AllowTransformationWhileCompacting(2, 4, 16);

        //options_.target_file_size_base = 64ul * 1024 * 1024;
        //options_.write_buffer_size = 2 << 30;
        //options_.db_write_buffer_size = 2 << 30;

        options_.level0_file_num_compaction_trigger = 2;
        options_.level0_slowdown_writes_trigger = 3;     
        options_.level0_stop_writes_trigger = 5;

        //options_.use_direct_reads = true;
        //options_.use_direct_io_for_flush_and_compaction = true;

        //options_.max_open_files = 20480;
        //options_.max_file_opening_threads = 32;

        options_.compaction_style = ROCKSDB_NAMESPACE::kCompactionStyleNone;
        options_.IncreaseParallelism(5);
    }

    void Mycelium::KeepOnlyRequestedFields(data::Row &row,
                    const std::vector<std::string> *fields, data::Row &selectedColumns)
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

    void Mycelium::GetColumnFamilyDescriptors(const std::string& dbname, std::vector<rocksdb::ColumnFamilyDescriptor>& column_families)
    {
        options_.SetCompactingLevelWithinColumnFamilyGroup(0);
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(
                        dbname, rocksdb::ColumnFamilyOptions(options_)));
        
        int level = 1;
        int splits = 1;
        while (level < options_.compacting_column_family_num_levels) {
            splits *= 2;
            if (level == options_.compacting_column_family_num_levels - 1 || splits > options_.num_columns) {
                splits = options_.num_columns;
            }
            for (int i= 0; i < splits; i++) {
                std::string cf_name = dbname + "_sys_cf_" + std::to_string(level) + "_" + std::to_string(i);
                options_.SetCompactingLevelWithinColumnFamilyGroup(level);
                column_families.push_back(rocksdb::ColumnFamilyDescriptor(cf_name, rocksdb::ColumnFamilyOptions(options_)));
            }
            
            level += 1;
        }
    }

    void Mycelium::BuildColumnFamilyHandleMap(std::vector<rocksdb::ColumnFamilyDescriptor>& column_family_descriptors,
                                              std::vector<rocksdb::ColumnFamilyHandle*> handles)
    {
        for (size_t i = 0; i < handles.size(); i++) {
            cfhandles_.insert({column_family_descriptors[i].name, handles[i]});
        }
    }
}
