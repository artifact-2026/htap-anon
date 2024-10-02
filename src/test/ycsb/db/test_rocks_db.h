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
#include "proto/columns.pb.h"

namespace ycsbc {

class TestRocksDB : public DB{
    public :
        TestRocksDB(const std::string& dbname, const char *dbfilename, utils::Properties &props);
        int Read(const std::string &table, const std::string &key,
                 const std::set<std::string> *fields,
                 std::string &result);

        int Scan(const std::string &table, const std::string &begin_key,
                 const std::string &end_key, const std::set<std::string> *fields,
                 std::vector<std::string> &result);

        int Insert(const std::string &table, const std::string &key,
                   std::string &values);

        int Update(const std::string &table, const std::string &key,
                   std::string &values);

        int Delete(const std::string &table, const std::string &key);

        ~TestRocksDB() {};
    
    private:
        rocksdb::DB *rocksdb_;
        rocksdb::Options options_;
        int noResults;
        std::shared_ptr<rocksdb::Cache> cache_;
        std::shared_ptr<rocksdb::Statistics> dbstats_;
        rocksdb::ColumnFamilyHandle* cfhandle_;

        void SetOptions(utils::Properties &props, bool logging, int levels, int fieldcount);
        void GetColumnFamilyDescriptors(const std::string& dbname, std::vector<rocksdb::ColumnFamilyDescriptor>& column_families);
        void BuildColumnFamilyHandles(std::vector<rocksdb::ColumnFamilyDescriptor> &column_family_descriptors,
                                                std::vector<rocksdb::ColumnFamilyHandle *> handles);        
};  

}

#endif
