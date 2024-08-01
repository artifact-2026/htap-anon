#ifndef YCSB_CPLUSPLUS_FLAT_BUFFERS_AND_CRACKING_H
#define YCSB_CPLUSPLUS_FLAT_BUFFERS_AND_CRACKING_H

#include "core/db.h"

#include <iostream>
#include <errno.h>
#include <string>

#include <rocksdb/options.h>
#include <rocksdb/db.h>
#include <rocksdb/cache.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/table.h>

#include "core/properties.h"
#include "core/core_workload.h"
#include "proto/columns.pb.h"

namespace ycsbc {

class TestFBCracker : public DB{
    public :
        TestFBCracker(const std::string& dbname, const char *dbfilename, utils::Properties &props);
        int Read(const std::string &table, const std::string &key,
                 const std::set<std::string> *fields,
                 std::string &result);

        int Scan(const std::string &table, const std::string &begin_key,
                 int32_t len, const std::set<std::string> *fields,
                 std::vector<std::string> &result);

        int Insert(const std::string &table, const std::string &key,
                   std::string &values);

        int Update(const std::string &table, const std::string &key,
                   std::string &values);

        int Delete(const std::string &table, const std::string &key);

        ~TestFBCracker() {};
    
    private:
        rocksdb::DB *rocksdb_;
        rocksdb::Options options_;
        std::map<std::string, rocksdb::ColumnFamilyHandle*> cfhandles_;
        std::vector<rocksdb::ColumnFamilyHandle*> cfhandlelist_;
        std::map<int, std::vector<rocksdb::ColumnFamilyHandle*>> cached_cfhandles_;
        int noResults;
        //std::shared_ptr<rocksdb::Cache> cache_;
        //std::shared_ptr<rocksdb::Statistics> dbstats_;

        void SetOptions(utils::Properties &props, bool logging, int levels, int fieldcount);
        void KeepOnlyRequestedFields(data::Row &row,
                const std::set<std::string> *fields, data::Row &selectedColumns);
        void BuildColumnFamilyHandles(std::vector<rocksdb::ColumnFamilyDescriptor> &column_family_descriptors,
                                    std::vector<rocksdb::ColumnFamilyHandle *> handles);
        void BuildQueryHandles(std::set<std::string> fields);
        void GetColumnFamilyDescriptors(const std::string &dbname,
                                             std::vector<rocksdb::ColumnFamilyDescriptor> &column_families);
};  

}

#endif