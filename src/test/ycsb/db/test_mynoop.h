#ifndef YCSB_C_MYCELIUM_NOOP_H
#define YCSB_C_MYCELIUM_NOOP_H

#include "core/db.h"

#include <iostream>
#include <errno.h>
#include <string>

#include <rocksdb/options.h>
#include <rocksdb/db.h>
#include <rocksdb/cache.h>
#include <rocksdb/table.h>
#include <rocksdb/transformer.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/mym_broker.h>

#include "core/properties.h"
#include "core/core_workload.h"
#include "data.pb.h"

#include "db/db_helper.h"

namespace ycsbc {

class TestMynoop : public DB{
    public :
        TestMynoop(const std::string& dbname, const char *dbfilename, utils::Properties &props);
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

        ~TestMynoop() {};
    
    private:
        std::unique_ptr<rocksdb::MymBroker> mymBroker_;
};  

}

#endif