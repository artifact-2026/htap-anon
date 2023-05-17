#ifndef YCSB_C_CABINDB_DB_H
#define YCSB_C_CABINDB_DB_H

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
#include "cabindb/cabin_db.h"
#include "cabindb/cabindb_namespace.h"

namespace ycsbc {

class CabinDB : public DB{
    public :
        CabinDB(const char *dbfilename, utils::Properties &props);
        int Read(const std::string &table, const std::string &key,
                 const std::vector<std::string> *fields,
                 std::vector<KVPair> &result);

        int Scan(const std::string &table, const std::string &key,
                 int len, const std::vector<std::string> *fields,
                 std::vector<std::vector<KVPair> > &result);

        int Insert(const std::string &table, const std::string &key,
                   std::vector<KVPair> &values);

        int Update(const std::string &table, const std::string &key,
                   std::vector<KVPair> &values);

        int Delete(const std::string &table, const std::string &key);

        ~CabinDB() {};
    
    private:
        cabindb::CabinDB* cabindb_;
        std::vector<std::string> cfshards_;
        unsigned noResultsInDefaultColumnFamily;
        unsigned noResults;
        std::shared_ptr<rocksdb::Cache> cache_;
        std::shared_ptr<rocksdb::Statistics> dbstats_;

        void SetOptions(rocksdb::Options *options, utils::Properties &props);
        void SerializeValue(std::vector<KVPair> &kvs, std::string &value);
        // de-serialize one row result for read
        void DeSerializeValue(std::string &value, std::vector<KVPair> &kvs);
        // de-serialize multiple rows result for scan
        void DeSerializeValues(std::vector<std::string> &values, std::vector<std::vector<KVPair>> &kvs_vec);
        // de-serialize multiple results for one key searched in columnar storage
        void StitchColumns(std::vector<std::string> &values, std::vector<KVPair> &kvs);

};

}

#endif
