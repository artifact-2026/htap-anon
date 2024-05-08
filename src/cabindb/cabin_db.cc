#include <iostream>
#include <cmath>
#include <queue>

#include "cabin_db.h"
#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "rocksdb-rados-env/env_librados.h"
#include "transformer/cracker.h"
#include "compactor.h"

namespace ROCKSDB_NAMESPACE {
    class CabinDBLogger : public rocksdb::Logger {
      public:
        explicit CabinDBLogger() {};
        ~CabinDBLogger() override {};

        void Logv(const char* format, va_list ap) override {
            Logv(rocksdb::INFO_LEVEL, format, ap);
        }

        void Logv(const rocksdb::InfoLogLevel log_level, const char* format, va_list ap) override {
            //dout(ceph::dout::need_dynamic(v));
            char buf[65536];
            vsnprintf(buf, sizeof(buf), format, ap);
           // *_dout << buf << dendl;
        }

    };

   CabinDB::CabinDB(const std::string& dbname,
                    const char *dbfilename,
                    bool bootstrap,
                    bool transform,
                    std::string translevel)
   {
    SetOptions(dbfilename);
    //rocksdb::CabinCompactor* compactor = new rocksdb::CabinCompactor(options_, dbname);
    //options_.listeners.emplace_back(compactor);

    if (transform) {
        options_.transformer = std::make_shared<rocksdb::Cracker>();
        options_.transform_type = 0;
        options_.translevel = translevel;
    }

    std::vector<rocksdb::ColumnFamilyDescriptor> column_family_descriptors;
    GetColumnFamilyDescriptors(dbname, column_family_descriptors, translevel);
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
        column_family_descriptors.push_back(rocksdb::ColumnFamilyDescriptor(
            rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions(options_)));
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
    //compactor->SetColumnFamilyHandles(cfhandles_);
   }

   int CabinDB::Read(const std::string &table, const std::string &key, const std::vector<std::string> *fields,
                      std::string &result)
   {
    auto it = cfhandles_.find(table);
    if (it != cfhandles_.end()) {
        rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(),
                                  it->second,
                                  key,
                                  &result);
        
        if (s.ok()) {
            return 0;
        }
    }
    return 1;
   }

    int CabinDB::Scan(const std::string &table, const std::string &begin_key,
                          int32_t len, const std::vector<std::string> *fields,
                          std::vector<std::string> &result)
    {
        result.clear();
        auto ith = cfhandles_.find(table);
        if (ith != cfhandles_.end()) {
            auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), ith->second);
            it->Seek(begin_key);
            for (int i = 0; i < len && it->Valid(); i++) {
                std::string value = it->value().ToString();

	            if (fields != NULL) {
                    data::Row row;
                    row.ParseFromString(value);
                    data::Row selectedColumns;
                    KeepOnlyRequestedFields(row, fields, selectedColumns);
                    std::string stitchedValue;
                    selectedColumns.SerializeToString(&stitchedValue);
                    result.push_back(stitchedValue);
	            } else {
	                result.push_back(value);
                }	      
                it->Next();
            }
        }
        
        return result.size();
    }

    int CabinDB::Insert(const std::string &table, const std::string &key, std::string &values)
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

    int CabinDB::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int CabinDB::Delete(const std::string &table, const std::string &key)
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

    CabinDB::~CabinDB()
    {
        rocksdb::Status s;
        for (auto handle : cfhandles_) {
            s = rocksdb_->DestroyColumnFamilyHandle(handle.second);
        }
        delete rocksdb_;
    }

    void CabinDB::SetOptions(const char *dbfilename)
    {
        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;

        /*
        std::string config_path = "/etc/ceph/ceph.conf";
        std::string rados_pool;
        rados_pool.append(dbfilename).append("_pool");
        options_.env = new rocksdb::EnvLibrados(dbfilename, config_path, rados_pool);
        */

        //options_.max_background_jobs = 16;
        //options_.max_write_buffer_number = 32;
        options_.AllowTransformationWhileCompacting(2, 4, 16, 1);
        options_.SetTransformType(1);

        //options_.target_file_size_base = 64ul * 1024 * 1024;
        //options_.write_buffer_size = 2 << 30;
        //options_.db_write_buffer_size = 2 << 30;

        //options_.level0_file_num_compaction_trigger = 2;
        //options_.compaction_style = ROCKSDB_NAMESPACE::kCompactionStyleNone;
        options_.IncreaseParallelism(16);
        options_.level0_slowdown_writes_trigger = 9999999;     
        options_.level0_stop_writes_trigger = 99999999;
        options_.max_open_files = -1;

        options_.use_direct_reads = true;
        options_.use_direct_io_for_flush_and_compaction = true;

        //options_.max_open_files = 20480;
        //options_.max_file_opening_threads = 32;

        
    }

    void CabinDB::KeepOnlyRequestedFields(data::Row &row,
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

    void CabinDB::GetColumnFamilyDescriptors(const std::string& dbname, 
                                             std::vector<rocksdb::ColumnFamilyDescriptor>& column_families,
                                             std::string translevel)
    {
        
        options_.SetCompactingLevelWithinColumnFamilyGroup(0);
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(
                        dbname, rocksdb::ColumnFamilyOptions(options_)));

        int level = 1;
        int splits = 2;
        int columns = options_.num_columns;
        std::queue<std::string> parents;
        parents.push(dbname+"_sys_cf");

/*
        for (int i = 0; i < options_.num_columns; i++) {
            std::string cf_name = parent_name + "_level-" + std::to_string(level) + "-" + std::to_string(j);
            options_.SetCompactingLevelWithinColumnFamilyGroup(level);
            column_families.push_back(rocksdb::ColumnFamilyDescriptor(cf_name, rocksdb::ColumnFamilyOptions(options_)));
            parents.push(cf_name);
        }
*/

        while (level < options_.compacting_column_family_num_levels) {
            if (columns > 1) {
                if (level == options_.compacting_column_family_num_levels - 1) {
                    splits = columns;
                }
                
                int queueLen = parents.size();

                for (int i = 0; i < queueLen; i++) {
                    std::string parent_name = parents.front();
                    parents.pop();
                    for (int j= 0; j < splits; j++) {
                        std::string cf_name = parent_name + "_level-" + std::to_string(level) + "-" + std::to_string(j);
                        options_.SetCompactingLevelWithinColumnFamilyGroup(level);
                        column_families.push_back(rocksdb::ColumnFamilyDescriptor(cf_name, rocksdb::ColumnFamilyOptions(options_)));
                        parents.push(cf_name);
                    }
                }
            }

            columns /= splits;
            level += 1;
        }
        
    }

    void CabinDB::BuildColumnFamilyHandleMap(
                                std::vector<rocksdb::ColumnFamilyDescriptor>& column_family_descriptors,
                                std::vector<rocksdb::ColumnFamilyHandle*> handles)
    {
        for (size_t i = 0; i < handles.size(); i++) {
            cfhandles_.insert({column_family_descriptors[i].name, handles[i]});
        }
    }

} // namespace CABINDB_NAMESPACE
