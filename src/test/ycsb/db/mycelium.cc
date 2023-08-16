#include "core/core_workload.h"
#include "mycelium.h"
#include "lib/coding.h"
#include "cabindb/rocksdb-rados-env/env_librados.h"
#include "cabindb/compactor.h"

using namespace std;

namespace ycsbc {
    Mycelium::Mycelium(const char *dbfilename, utils::Properties &props) {
        noResults = 0;
        SetOptions(props, dbfilename);
        rocksdb::Status s = rocksdb::DB::Open(options_, 
                                              dbfilename,
                                              &rocksdb_);
    
        /*
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","true"));
        int num_cfs = stoi(props.GetProperty("fieldcount", "0")) + 6;

        if (bootstrap) {
            rocksdb::Status s = rocksdb::DB::Open(options_, 
                                              dbfilename,
                                              &rocksdb_);
            if (!s.ok()){
                std::cerr<<"Can't open mycelium "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }

            cfhandles_map_[rocksdb::kDefaultColumnFamilyName] = rocksdb_->DefaultColumnFamily();
            for (int i = 0; i < num_cfs; i++) {
                rocksdb::ColumnFamilyHandle* cf;
                std::string cfname = "cf"+std::to_string(i+1);
                s = rocksdb_->CreateColumnFamily(rocksdb::ColumnFamilyOptions(), cfname, &cf);
                cfhandles_map_[cfname] = cf;
            }
        } else {
            std::vector<rocksdb::ColumnFamilyDescriptor> column_families;
            column_families.push_back(rocksdb::ColumnFamilyDescriptor(
                        rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions()));
            for (int i = 0; i < num_cfs; i++) {
                std::string cfname = "cf"+std::to_string(i+1);
                column_families.push_back(
                        rocksdb::ColumnFamilyDescriptor(cfname, rocksdb::ColumnFamilyOptions()));
            }
            std::vector<rocksdb::ColumnFamilyHandle*> handles;

            rocksdb::Status s = rocksdb::DB::Open(options_,
                                              dbfilename,
                                              column_families,
                                              &handles,
                                              &rocksdb_);
            if (!s.ok()){
                std::cerr<<"Can't open mycelium "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }

            cfhandles_map_[rocksdb::kDefaultColumnFamilyName] = rocksdb_->DefaultColumnFamily();
            for (int i = 0; i < num_cfs; i++) {
                std::string cfname = "cf"+std::to_string(i+1);
                cfhandles_map_[cfname] = handles[i+1];
            }
        }
        */
    }

    /*
    * Read is for point query over all columns
    */
    int Mycelium::Read(const std::string &table, const std::string &key, const std::vector<std::string> *fields,
                      data::Row &result) 
    {
        std::string value;
        rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(),
                                          //cfhandles_map_[rocksdb::kDefaultColumnFamilyName],
                                          key,
                                          &value);
        
        if (s.ok()) {
            result.ParseFromString(value);
            return 0;
        }

        noResults++;
        return 1;
    }

    int Mycelium::Scan(const std::string &table, const std::string &begin_key,
                          int32_t len, const std::vector<std::string> *fields,
                          std::vector<data::Row> &result) 
    {
        result.clear();
        auto it = rocksdb_->NewIterator(rocksdb::ReadOptions());
                                        //cfhandles_map_[rocksdb::kDefaultColumnFamilyName]);
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

    int Mycelium::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        rocksdb::Status s = rocksdb_->Put(rocksdb::WriteOptions(),
                                          //cfhandles_map_[rocksdb::kDefaultColumnFamilyName],
                                          key,
                                          values);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    int Mycelium::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int Mycelium::Delete(const std::string &table, const std::string &key)
    {
        rocksdb::Status s = rocksdb_->Delete(rocksdb::WriteOptions(),
                                             cfhandles_map_[rocksdb::kDefaultColumnFamilyName],
                                             key);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    void Mycelium::SetOptions(utils::Properties &props, const char *dbfilename)
    {
        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;

        std::string config_path = "/etc/ceph/ceph.conf";
        std::string rados_pool;
        rados_pool.append(dbfilename).append("_pool");
        options_.env = new rocksdb::EnvLibrados(dbfilename, config_path, rados_pool);
        options_.num_levels = 3;

        options_.max_background_jobs = 16;
        options_.max_write_buffer_number = 32;
        //options_.target_file_size_base = 64ul * 1024 * 1024;
        //options_.write_buffer_size = 2 << 30;
        //options_.db_write_buffer_size = 2 << 30;

        //options_.level0_file_num_compaction_trigger = 8;
        //options_.level0_slowdown_writes_trigger = -1;     
        //options_.level0_stop_writes_trigger = -1;

        //options_.use_direct_reads = true;
        //options_.use_direct_io_for_flush_and_compaction = true;

        //options_.max_open_files = 20480;
        //options_.max_file_opening_threads = 32;
        options_.compaction_style = ROCKSDB_NAMESPACE::kCompactionStyleNone;
        options_.IncreaseParallelism(5);
        options_.listeners.emplace_back(new rocksdb::CabinCompactor(options_));
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

}
