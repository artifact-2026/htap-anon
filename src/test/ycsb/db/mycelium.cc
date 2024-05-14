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
        std::string finalResult;
        bool found = false;
        int level = 0;
        std::string table_name;

        //if (fields == nullptr)
        //{
            int idx = 0; 
            int lvl = 1;
            int totalHdls = leveled_cfhandles_.size();
            while (!found && level < options_.num_levels)
            {
                while (idx < totalHdls && idx < 2*lvl-1) {
                    rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(),
                                                          leveled_cfhandles_[idx],
                                                          key,
                                                          &result);
                    if (s.ok() && result != "") {
                        found = true;
                    } else {
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
        //} 
        /*else {
            std::string tab;
            while (!found && level < options_.compacting_column_family_num_levels)
            {
                if (level == 0) {
                    tab = table;
                } else {
                    tab = table_name + std::to_string(level) + "-" + std::to_string(0);
                }
                
                auto it = cfhandles_.find(tab);
                if (it != cfhandles_.end())
                {
                    rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(),
                                                          it->second,
                                                          key,
                                                          &result);

                    if (result != "") {
                        found = true;
                    }
                }
                
                level++;
            }
        }*/
        return 1;
    }

    int Mycelium::Scan(const std::string &table, const std::string &begin_key,
                          int32_t len, const std::set<std::string> *fields,
                          std::vector<std::string> &result) 
    {
        result.clear();
        auto ith = cfhandles_.find(table);
        if (ith != cfhandles_.end())
        {
            auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), ith->second);
            it->Seek(begin_key);
            for (int i = 0; i < len && it->Valid(); i++)
            {
                std::string value = it->value().ToString();

                if (fields != NULL)
                {
                    data::Row row;
                    row.ParseFromString(value);
                    data::Row selectedColumns;
                    KeepOnlyRequestedFields(row, fields, selectedColumns);
                    std::string stitchedValue;
                    selectedColumns.SerializeToString(&stitchedValue);
                    result.push_back(stitchedValue);
                }
                else
                {
                    result.push_back(value);
                }
                it->Next();
            }
        }

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

        int level = 1;
        int splits = 2;
        int columns = options_.num_columns;
        std::queue<std::string> parents;
        parents.push(dbname + "_sys_cf");

        /*
                for (int i = 0; i < options_.num_columns; i++) {
                    std::string cf_name = parent_name + "_level-" + std::to_string(level) + "-" + std::to_string(j);
                    options_.SetCompactingLevelWithinColumnFamilyGroup(level);
                    column_families.push_back(rocksdb::ColumnFamilyDescriptor(cf_name, rocksdb::ColumnFamilyOptions(options_)));
                    parents.push(cf_name);
                }
        */

        while (level < options_.compacting_column_family_num_levels)
        {
            if (columns > 1)
            {
                if (level == options_.compacting_column_family_num_levels - 1)
                {
                    splits = columns;
                }

                int queueLen = parents.size();

                for (int i = 0; i < queueLen; i++)
                {
                    std::string parent_name = parents.front();
                    parents.pop();
                    for (int j = 0; j < splits; j++)
                    {
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

    void Mycelium::BuildColumnFamilyHandleMap(
        std::vector<rocksdb::ColumnFamilyDescriptor> &column_family_descriptors,
        std::vector<rocksdb::ColumnFamilyHandle *> handles)
    {
        for (size_t i = 0; i < handles.size(); i++)
        {
            cfhandles_.insert({column_family_descriptors[i].name, handles[i]});
        }
    }

}
