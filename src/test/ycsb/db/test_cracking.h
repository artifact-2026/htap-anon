#ifndef YCSB_CPLUSPLUS_MYCELIUM_H
#define YCSB_CPLUSPLUS_MYCELIUM_H

#include "core/db.h"

#include <iostream>
#include <errno.h>
#include <string>
#include <vector>

#include <rocksdb/options.h>
#include <rocksdb/db.h>
#include <rocksdb/cache.h>
#include <rocksdb/table.h>
#include <rocksdb/filter_policy.h>

#include "core/properties.h"
#include "core/core_workload.h"
#include "proto/columns.pb.h"

#include "db/db_helper.h"

namespace ycsbc {

class Mycelium : public DB{
    public :
        Mycelium(const std::string& dbname, const char *dbfilename, utils::Properties &props);
        int Read(const std::string &table, const std::string &key,
                 const std::set<std::string> *fields,
                 const std::string &req_dist, bool index_access,
                 std::string &result);

        int Scan(const std::string &table, const std::string &begin_key,
                 const std::string &end_key, const std::set<std::string> *fields,
                 const std::string &req_dist, bool index_access,
                 std::vector<std::string> &result);

        int Insert(const std::string &table, const std::string &key,
                   std::string &values);

        int Update(const std::string &table, const std::string &key,
                   std::string &values);

        int Delete(const std::string &table, const std::string &key);

        ~Mycelium() {};
    
    private:
        rocksdb::DB *rocksdb_;
        rocksdb::Options options_;
        rocksdb::WriteOptions write_options_;
        std::map<std::string, rocksdb::ColumnFamilyHandle*> cfhandles_;
        std::vector<rocksdb::ColumnFamilyHandle*> cfhandlelist_;
        std::map<int, std::vector<rocksdb::ColumnFamilyHandle*>> cached_cfhandles_;

        void SetOptions(const char *dbfilename, bool logging, int levels, int fieldcount,
                        rocksdb::InputOutputDataType inputType,
                        rocksdb::InputOutputDataType outputType);
        void GetColumnFamilyDescriptors(const std::string& dbname,
                                    std::vector<rocksdb::ColumnFamilyDescriptor>& column_families,
                                    int num_splits);
        void BuildColumnFamilyHandleMap(std::vector<rocksdb::ColumnFamilyDescriptor>& column_family_descriptors,
                                    std::vector<rocksdb::ColumnFamilyHandle*> handles);
        void BuildQueryHandles(std::set<std::string> fields);
        rocksdb::Status PerformGet(rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cfHandle, 
                                   const std::string& key, std::string& result);
};  

}

#endif
