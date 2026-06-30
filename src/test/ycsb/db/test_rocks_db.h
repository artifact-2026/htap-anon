#ifndef YCSB_C_ROCKS_DB_H
#define YCSB_C_ROCKS_DB_H

#include "core/db.h"

#include <iostream>
#include <errno.h>
#include <string>

#include <rocksdb/options.h>
#include <rocksdb/db.h>
#include <rocksdb/cache.h>
#include <rocksdb/table.h>
#include <rocksdb/filter_policy.h>

#include "core/properties.h"
#include "core/core_workload.h"
#include "data.pb.h"

#include "db/db_helper.h"
#include "db/compaction_metrics_listener.h"

namespace ycsbc {

class TestRocksDB : public DB{
    public :
        TestRocksDB(const std::string& dbname, const char *dbfilename, utils::Properties &props);
        int Read(const std::string &table, const std::string &key,
                 const std::set<int> *fields,
                 const std::string &req_dist, bool index_access,
                 std::string &result);

        int Scan(const std::string &table, const std::string &begin_key,
                 const std::string &end_key, const std::set<int> *fields,
                 const std::string &req_dist, bool index_access,
                 std::vector<std::string> &result);

        int Insert(const std::string &table, const std::string &key,
                   std::string &values);

        int Update(const std::string &table, const std::string &key,
                   std::string &values);

        int Delete(const std::string &table, const std::string &key);

        ~TestRocksDB() {
            if (!rocksdb_) return;

            for (auto* h : cfhandles_) {
                if (h) rocksdb_->DestroyColumnFamilyHandle(h);
            }

            cfhandles_.clear();
            default_cf_ = nullptr;
            cfhandle_ = nullptr;
            
            delete rocksdb_;
            rocksdb_ = nullptr;
          
        };
    
    private:
        rocksdb::DB *rocksdb_;
        rocksdb::Options options_;
        rocksdb::WriteOptions write_options_;
        int noResults;
        std::shared_ptr<rocksdb::Cache> cache_;
        std::shared_ptr<rocksdb::Statistics> dbstats_;
        rocksdb::ColumnFamilyHandle* default_cf_{nullptr};
        rocksdb::ColumnFamilyHandle* cfhandle_{nullptr};
        std::vector<rocksdb::ColumnFamilyHandle*> cfhandles_;
        std::string inputType_;
        std::string outputType_;
        std::string columnDataType_;

        // Compaction metrics collection (enabled via "metrics_output" property)
        std::shared_ptr<CompactionMetricsListener> metrics_listener_;

        void SetOptions(utils::Properties &props, bool logging, int levels, int fieldcount);
        void GetColumnFamilyDescriptors(const std::string& dbname, std::vector<rocksdb::ColumnFamilyDescriptor>& column_families);
        void BuildColumnFamilyHandles(std::vector<rocksdb::ColumnFamilyDescriptor>& column_family_descriptors,
                                                std::vector<rocksdb::ColumnFamilyHandle*>& handles);        
};  

}

#endif
