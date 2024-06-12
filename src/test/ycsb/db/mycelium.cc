#include <iostream>
#include <cmath>
#include <queue>

#include "core/core_workload.h"
#include "mycelium.h"
#include "lib/coding.h"

#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "transformer/cracker.h"
#include "cabindb/compactor.h"

using namespace std;

namespace ycsbc {
    Mycelium::Mycelium(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","true"));
        bool transform = utils::StrToBool(props.GetProperty("transform","true"));
        std::string translevel = props.GetProperty("translevel","all");
        SetOptions(dbfilename);

        if (transform) {
            options_.transformer = std::make_shared<rocksdb::Cracker>();
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
        //cabindb_ = new rocksdb::CabinDB(dbname, dbfilename, bootstrap, transform, translevel);
    }

    /*
    * Read is for point query over all columns
    */
    int Mycelium::Read(const std::string &table, const std::string &key, const std::set<std::string> *fields,
                      std::string &result)
    {
        /*std::string value;
        std::queue<std::string> children;
        children.push(table);

        for (int i = 0; i < 4; i++) {
            int queueLen = childen.size();

            for (int j = 0; j < queueLen; j++) {
                std::string tab = children.front();
                children.pop();

                auto it = cfhandles_.find(tab);
                if (it == cfhandles_.end()) {
                    return 1;
                }

                rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(),
                                              it->second,
                                              key,
                                              value);

            }
        }*/
       
        bool found = false;
        int level = 0;
        int idx = 0; 
        int lvl = 1;
        int totalHdls = leveled_cfhandles_.size();
        data::Row selectedColumns;
        std::set<std::string> modifiableFields; // Make a copy
        if (fields != nullptr) {
            modifiableFields = *fields;
        }
  
        while (!found && level < options_.compacting_column_family_num_levels) {
            while (idx < totalHdls && idx < 2*lvl-1) {
                std::string partial;
                rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(),
                                                      leveled_cfhandles_[idx],
                                                      key,
                                                      &partial);
                if (partial == "") {
                    idx = 2*lvl - 1;
                    break;
                }
                
                if (!found) {
                    found = true;
                }

                if (modifiableFields.size() == 0) {
                    result += partial;
                    idx++;
                    continue;
                }

                data::Row row;
                row.ParseFromString(partial);
                for (int i = 0; i < row.columns_size(); i++) {
                    modifiableFields.erase(row.columns(i).name());
                    data::Column* selectedColumn = selectedColumns.add_columns();
                    selectedColumn->set_name(row.columns(i).name());
                    selectedColumn->set_value(row.columns(i).value());
                }

                if (modifiableFields.size() == 0) {
                    break;
                }

                idx++;
            }

            level++;
            lvl *= 2;

            if (idx >= totalHdls) {
                break;
            }
        }

        if (found && result == "") {
            selectedColumns.SerializeToString(&result);
        }
        
        return 1;
    }

    int Mycelium::Scan(const std::string &table, const std::string &begin_key,
                          int32_t len, const std::set<std::string> *fields,
                          std::vector<std::string> &result) 
    {
        /*result.clear();
        auto ith = cfhandles_.find(table);
        if (ith != cfhandles_.end())
        {
            auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), ith->second);
            it->Seek(begin_key);

            int totalHdls = leveled_cfhandles_.size();
            
            for (int i = 0; i < len && it->Valid(); i++)
            {
                std::string value = it->value().ToString();
                bool found = false;
                int level = 0;
                int idx = 0; 
                int lvl = 1;
                std::set<std::string> modifiableFields; // Make a copy
                if (fields != nullptr) {
                    modifiableFields = *fields;
                }
                data::Row selectedColumns;

                while (!found && level < options_.compacting_column_family_num_levels) {
                    while (idx < totalHdls && idx < 2*lvl-1) {
                        std::string partial;
                        rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(),
                                                      leveled_cfhandles_[idx],
                                                      begin_key,
                                                      &partial);
                        if (partial == "") {
                            idx = 2*lvl - 1;
                            break;
                        }
                
                        if (!found) {
                            found = true;
                        }

                        if (modifiableFields.size() == 0) {
                            result += partial;
                            idx++;
                            continue;
                        }

                        data::Row row;
                        row.ParseFromString(partial);
                        for (int i = 0; i < row.columns_size(); i++) {
                            modifiableFields.erase(row.columns(i).name());
                            data::Column* selectedColumn = selectedColumns.add_columns();
                            selectedColumn->set_name(row.columns(i).name());
                            selectedColumn->set_value(row.columns(i).value());
                        }

                        if (modifiableFields.size() == 0) {
                            break;
                        }

                        idx++;
                    }

                    level++;
                    lvl *= 2;

                    if (idx >= totalHdls) {
                        break;
                    }
                }

                it->Next();
            }
        }*/

        return result.size();
    }

    int Mycelium::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        auto it = cfhandles_.find(table);
        if (it != cfhandles_.end())
        {
            rocksdb::Status s = rocksdb_->Put(rocksdb::WriteOptions(),
                                              it->second,
                                              key,
                                              values);
            if (s.ok())
            {
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
        if (it != cfhandles_.end())
        {
            rocksdb::Status s = rocksdb_->Delete(rocksdb::WriteOptions(),
                                                 it->second,
                                                 key);
            if (s.ok())
            {
                return 0;
            }
        }
        return 1;
    }

    void Mycelium::SetOptions(const char *dbfilename)
    {
        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;

        /*
        std::string config_path = "/etc/ceph/ceph.conf";
        std::string rados_pool;
        rados_pool.append(dbfilename).append("_pool");
        options_.env = new rocksdb::EnvLibrados(dbfilename, config_path, rados_pool);
        */

        // options_.max_background_jobs = 16;
        // options_.max_write_buffer_number = 32;
        options_.AllowTransformationWhileCompacting(2, 4, 16, 1);
        options_.SetTransformType(1);

        // options_.target_file_size_base = 64ul * 1024 * 1024;
        // options_.write_buffer_size = 2 << 30;
        // options_.db_write_buffer_size = 2 << 30;

        // options_.level0_file_num_compaction_trigger = 2;
        // options_.compaction_style = ROCKSDB_NAMESPACE::kCompactionStyleNone;
        options_.IncreaseParallelism(16);
        options_.level0_slowdown_writes_trigger = 9999999;
        options_.level0_stop_writes_trigger = 99999999;
        options_.max_open_files = -1;

        options_.use_direct_reads = true;
        options_.use_direct_io_for_flush_and_compaction = true;

        // options_.max_open_files = 20480;
        // options_.max_file_opening_threads = 32;
    }

    void Mycelium::KeepOnlyRequestedFields(data::Row &row,
                                          const std::set<std::string> *fields, data::Row &selectedColumns)
    {
        for (auto field : *fields)
        {
            for (int i = 0; i < row.columns_size(); i++)
            {
                if (row.columns(i).name().compare(field) == 0)
                {
                    data::Column *selectedColumn = selectedColumns.add_columns();
                    selectedColumn->set_name(row.columns(i).name());
                    selectedColumn->set_value(row.columns(i).value());
                    break;
                }
            }
        }
    }

    void Mycelium::GetColumnFamilyDescriptors(const std::string &dbname,
                                             std::vector<rocksdb::ColumnFamilyDescriptor> &column_families,
                                             std::string translevel)
    {
        options_.SetCompactingLevelWithinColumnFamilyGroup(0);
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(
            dbname, rocksdb::ColumnFamilyOptions(options_)));
      
        std::string prefix = dbname + "_sys_cf";
        std::queue<int> parents;
        parents.push(options_.num_columns);

        for (int level = 1; level < options_.compacting_column_family_num_levels; level++) {
            int queueLen = parents.size();

            for (int j = 0; j < queueLen; j++) {
                int parent_cols = parents.front();
                parents.pop();
                if (parent_cols < 2) {
                    continue;
                }
                rocksdb::Options cfoptions = options_;
                cfoptions.SetCompactingLevelWithinColumnFamilyGroup(level);

                int child1 = parent_cols/2;
                std::string cfname1 = prefix + "_L" + std::to_string(level) + "_G" + std::to_string(j*2);
                column_families.push_back(rocksdb::ColumnFamilyDescriptor(cfname1, rocksdb::ColumnFamilyOptions(cfoptions)));
                parents.push(child1);

                int child2 = parent_cols - child1;
                std::string cfname2 = prefix + "_L" + std::to_string(level) + "_G" + std::to_string(j*2+1);
                column_families.push_back(rocksdb::ColumnFamilyDescriptor(cfname2, rocksdb::ColumnFamilyOptions(cfoptions)));
                parents.push(child2);
            }
        }
    }

    void Mycelium::BuildColumnFamilyHandleMap(
        std::vector<rocksdb::ColumnFamilyDescriptor> &column_family_descriptors,
        std::vector<rocksdb::ColumnFamilyHandle *> handles)
    {
        for (size_t i = 0; i < handles.size(); i++)
        {
            cfhandles_.insert({column_family_descriptors[i].name, handles[i]});
            leveled_cfhandles_.push_back(handles[i]);
        }
    }

}
