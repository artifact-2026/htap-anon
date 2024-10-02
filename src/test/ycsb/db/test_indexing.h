#ifndef YCSB_CPLUSPLUS_INDEXING_H
#define YCSB_CPLUSPLUS_INDEXING_H

#include "core/db.h"

#include <iostream>
#include <sstream>
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

#include "transformer/augmenter.h"

namespace ycsbc {

class Indexing : public DB{
    public :
        Indexing(const std::string& dbname, const char *dbfilename, utils::Properties &props);
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

        ~Indexing() {};
    
    private:
        rocksdb::DB *rocksdb_;
        rocksdb::Options options_;
        std::map<std::string, rocksdb::ColumnFamilyHandle*> cfhandles_;
        int noResults;

        void SetOptions(const char *dbfilename, bool logging, int levels, int fieldcount);
	    void KeepOnlyRequestedFields(data::Row &row,
                const std::set<std::string> *fields, data::Row &selectedColumns);
        std::vector<std::string> deserializeIndex(const std::string& serialized);
        void GetColumnFamilyDescriptors(const std::string& dbname,
                                        std::vector<rocksdb::ColumnFamilyDescriptor>& column_families);
        void BuildColumnFamilyHandleMap(std::vector<rocksdb::ColumnFamilyDescriptor>& column_family_descriptors,
                                    std::vector<rocksdb::ColumnFamilyHandle*> handles);
        rocksdb::DeriveFuncData* CreateIndexer(std::vector<int> positions);
};  

}

#endif
