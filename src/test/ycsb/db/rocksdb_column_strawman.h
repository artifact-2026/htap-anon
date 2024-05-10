#ifndef YCSB_CPLUSPLUS_ROCKSDB_COLUMN_STRAWMAN_H
#define YCSB_CPLUSPLUS_ROCKSDB_COLUMN_STRAWMAN_H

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
                 int32_t len, const std::set<std::string> *fields,
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

        void SetOptions(utils::Properties &props, const char *dbfilename, int num_cfs);
        // serialize for inserts
        void SerializeValue(std::vector<KVPair> &kvs, std::string &value);
        // de-serialize one record
        void DeSerializeValue(std::string &value,
                    const std::vector<std::string> *fields,
                    std::vector<KVPair> &kvs);
	    void KeepOnlyRequestedFields(data::Row &row,
                    const std::set<std::string> *fields, data::Row &selectedColumns);
        void GetColumnFamilyDescriptors(const std::string& dbname,
                    std::vector<rocksdb::ColumnFamilyDescriptor>& column_families);
        void BuildColumnFamilyHandleMap(std::vector<rocksdb::ColumnFamilyDescriptor>& column_family_descriptors,
                            std::vector<rocksdb::ColumnFamilyHandle*> handles);
};  

}

#endif