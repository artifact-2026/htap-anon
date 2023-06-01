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

class TestCabinDB : public DB{
    public :
        TestCabinDB(const char *dbfilename, utils::Properties &props);
        int Read(const std::string &table, const std::string &key,
                 const std::vector<std::string> *fields,
                 data::Row &result);

        int Scan(const std::string &table, const std::string &key,
                 int len, const std::vector<std::string> *fields,
                 std::vector<data::Row> &result);

        int Insert(const std::string &table, const std::string &key,
                   std::string &values);

        int Update(const std::string &table, const std::string &key,
                   std::string &values);

        int Delete(const std::string &table, const std::string &key);
	
        ~TestCabinDB() {};
    
    private:
        cabindb::CabinDB* cabindb_;
        unsigned noResultsInDefaultColumnFamily;
        unsigned noResults;
        std::map<std::string, std::map<int, int> > field_to_cfpositions_map_;
        std::shared_ptr<rocksdb::Cache> cache_;
        std::shared_ptr<rocksdb::Statistics> dbstats_;

        void SetOptions(rocksdb::Options *options, utils::Properties &props);
        void StitchColumns(std::vector<std::string> &values, std::vector<KVPair> &kvs);
        void PopulateFieldToColumnFamilyNameMap(utils::Properties &props);
        void GetColumnFamiliesOnOneLevel(const std::vector<std::string> *fields,
                                         int level, std::set<int> &cf_positions_on_the_level);

};

}

#endif
