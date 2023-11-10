#include "core/core_workload.h"
#include "mycelium.h"
#include "lib/coding.h"
#include "cabindb/rocksdb-rados-env/env_librados.h"
#include "cabindb/compactor.h"

using namespace std;

namespace ycsbc {
    Mycelium::Mycelium(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","true"));
        cabindb_ = new rocksdb::CabinDB(dbname, dbfilename, bootstrap);
    }

    /*
    * Read is for point query over all columns
    */
    int Mycelium::Read(const std::string &table, const std::string &key, const std::vector<std::string> *fields,
                      data::Row &result) 
    {
        int read = cabindb_->Read(table, key, fields, result);
        if (read != 0) {
            noResults++;
        }
        return read;
    }

    int Mycelium::Scan(const std::string &table, const std::string &begin_key,
                          int32_t len, const std::vector<std::string> *fields,
                          std::vector<data::Row> &result) 
    {
        cabindb_->Scan(table, begin_key, len, fields, result);
        return result.size();
    }

    int Mycelium::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        return cabindb_->Insert(table, key, values);
    }

    int Mycelium::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int Mycelium::Delete(const std::string &table, const std::string &key)
    {
        return cabindb_->Delete(table, key);
    }

}
