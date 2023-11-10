#ifndef YCSB_CPLUSPLUS_MYCELIUM_H
#define YCSB_CPLUSPLUS_MYCELIUM_H

#include "core/db.h"

#include <iostream>
#include <errno.h>
#include <string>

#include "cabindb/cabin_db.h"
#include "core/properties.h"
#include "core/core_workload.h"
#include "proto/columns.pb.h"

namespace ycsbc {

class Mycelium : public DB{
    public :
        Mycelium(const std::string& dbname, const char *dbfilename, utils::Properties &props);
        int Read(const std::string &table, const std::string &key,
                 const std::vector<std::string> *fields,
                 data::Row &result);

        int Scan(const std::string &table, const std::string &begin_key,
                 int32_t len, const std::vector<std::string> *fields,
                 std::vector<data::Row> &result);

        int Insert(const std::string &table, const std::string &key,
                   std::string &values);

        int Update(const std::string &table, const std::string &key,
                   std::string &values);

        int Delete(const std::string &table, const std::string &key);

        ~Mycelium() {};
    
    private:
        rocksdb::CabinDB *cabindb_;
        int noResults; 
};  

}

#endif
