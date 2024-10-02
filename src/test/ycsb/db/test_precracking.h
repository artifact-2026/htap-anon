#ifndef YCSB_CPLUSPLUS_PRECRACKING_H
#define YCSB_CPLUSPLUS_PRECRACKING_H

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

class RocksdbColumnStrawman : public DB{
    public :
        RocksdbColumnStrawman(const std::string& dbname, const char *dbfilename, utils::Properties &props);
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

        ~RocksdbColumnStrawman() {};
    
    private:
        rocksdb::DB *rocksdb_;
        rocksdb::Options options_;
        int noResults;
        std::map<std::string, rocksdb::ColumnFamilyHandle*> cfhandles_;
        std::vector<rocksdb::ColumnFamilyHandle*> handleList_;

        void SetOptions(const char *dbfilename, int levels, int fieldcount, bool logging);
	    void KeepOnlyRequestedFields(data::Row &row,
                    const std::set<std::string> *fields, data::Row &selectedColumns);
        void GetColumnFamilyDescriptors(const std::string& dbname,
                    std::vector<rocksdb::ColumnFamilyDescriptor>& column_families);
        void BuildColumnFamilyHandleMap(std::vector<rocksdb::ColumnFamilyDescriptor>& column_family_descriptors,
                            std::vector<rocksdb::ColumnFamilyHandle*> handles);
        std::set<int> GetQueryingHandles(std::set<std::string> fields);
};  

}

#endif