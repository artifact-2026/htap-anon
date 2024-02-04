#include <iostream>
#include <cmath>
#include <queue>
#include "core/core_workload.h"
#include "writetwice.h"
#include "lib/coding.h"
#include "cabindb/rocksdb-rados-env/env_librados.h"
#include "cabindb/compactor.h"
#include "lib/rocksdb/transformer/cracker.h"

using namespace std;

namespace ycsbc {
    Writetwice::Writetwice(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        noResults = 0;
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","true"));
        bool transform = utils::StrToBool(props.GetProperty("transform","true"));
        SetOptions(dbfilename);

        rocksdb::CabinCompactor* compactor = new rocksdb::CabinCompactor(options_);
        options_.listeners.emplace_back(compactor);

        if (transform) {
            options_.transformer = std::make_shared<rocksdb::Cracker>();
        }

        std::vector<rocksdb::ColumnFamilyDescriptor> column_family_descriptors;
        GetColumnFamilyDescriptors(dbname, column_family_descriptors);
        std::vector<rocksdb::ColumnFamilyHandle*> cf_handles;

        if (bootstrap) {
            rocksdb::Status s = rocksdb::DB::Open(options_, 
                                          dbfilename,
                                          &rocksdb_);
            if (!s.ok()){
                std::cerr<<"Can't open writetwice "<<dbfilename<<" "<<s.ToString()<<std::endl;
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
                std::cerr<<"Can't open writetwice "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }
        }
        BuildColumnFamilyHandleMap(column_family_descriptors, cf_handles);
        compactor->SetColumnFamilyHandles(cfhandles_);
    }

    /*
    * Read is for point query over all columns
    */
    int Writetwice::Read(const std::string &table, const std::string &key, const std::vector<std::string> *fields,
                      data::Row &result) 
    {
        std::string value;
        rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(),
                                          key,
                                          &value);
        
        if (s.ok()) {
            result.ParseFromString(value);
            return 0;
        }

        noResults++;
        return 1;
    }

    int Writetwice::Scan(const std::string &table, const std::string &begin_key,
                          int32_t len, const std::vector<std::string> *fields,
                          std::vector<data::Row> &result) 
    {
        result.clear();
        auto it = rocksdb_->NewIterator(rocksdb::ReadOptions());
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
        
        return result.size();
    }

    int Writetwice::Insert(const std::string &table, const std::string &key, std::string &values)
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

    int Writetwice::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int Writetwice::Delete(const std::string &table, const std::string &key)
    {
        rocksdb::Status s = rocksdb_->Delete(rocksdb::WriteOptions(), key);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    void Writetwice::SetOptions(const char *dbfilename)
    {
        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;

        options_.compaction_style = ROCKSDB_NAMESPACE::kCompactionStyleNone;
        options_.IncreaseParallelism(16);
        options_.level0_slowdown_writes_trigger = 9999999;
        options_.level0_stop_writes_trigger = 99999999;
        options_.max_open_files = -1;

        options_.AllowTransformationWhileCompacting(2, 4, 16);
        options_.write_both = true;

        //options_.max_background_jobs = 16;
        //options_.max_write_buffer_number = 32;
        //options_.target_file_size_base = 64ul * 1024 * 1024;
        //options_.write_buffer_size = 2 << 30;
        //options_.db_write_buffer_size = 2 << 30;

        //options_.level0_file_num_compaction_trigger = 8;
        //options_.level0_slowdown_writes_trigger = 16;     
        //options_.level0_stop_writes_trigger = 16;

        options_.use_direct_reads = true;
        options_.use_direct_io_for_flush_and_compaction = true;

        //options_.max_open_files = 20480;
        //options_.max_file_opening_threads = 32;
    }

    void Writetwice::KeepOnlyRequestedFields(data::Row &row,
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

    void Writetwice::GetColumnFamilyDescriptors(const std::string& dbname, std::vector<rocksdb::ColumnFamilyDescriptor>& column_families)
    {
        options_.SetCompactingLevelWithinColumnFamilyGroup(0);
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(
                        dbname, rocksdb::ColumnFamilyOptions(options_)));
        
        int level = 1;
        int splits = 2;
        int columns = options_.num_columns;
        std::queue<std::string> parents;
        parents.push(dbname+"_sys_cf");

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

        options_.SetCompactingLevelWithinColumnFamilyGroup(-1);
        std::string extra_cf_name = dbname + "_pure_storage";
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(extra_cf_name,
                                    rocksdb::ColumnFamilyOptions(options_)));
    }

    void Writetwice::BuildColumnFamilyHandleMap(std::vector<rocksdb::ColumnFamilyDescriptor>& column_family_descriptors,
                                              std::vector<rocksdb::ColumnFamilyHandle*> handles)
    {
        for (size_t i = 0; i < handles.size(); i++) {
            std::cout << "cf name: " << column_family_descriptors[i].name << std::endl;
            cfhandles_.insert({column_family_descriptors[i].name, handles[i]});
        }
    }

}
