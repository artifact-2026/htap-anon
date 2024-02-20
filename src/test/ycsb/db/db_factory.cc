#include "db/db_factory.h"

#include <string>
#include <iostream>
#include "db/test_rocks_db.h"
#include "db/mycelium.h"
#include "db/writetwice.h"
#include "db/rocksdb_column_strawman.h"
#include "db/splitfirst.h"

using namespace std;

namespace ycsbc {
//using ycsbc::DB;
//using ycsbc::DBFactory;

DB* DBFactory::CreateDB(utils::Properties &props) {
  if (props["dbname"] == "rocksdb") {
    std::string dbpath = props.GetProperty("dbpath","/tmp/test-rocksdb");
    return new TestRocksDB(dbpath.c_str(), props);
  } else if (props["dbname"] == "mycelium") {
    std::string dbpath = props.GetProperty("dbpath","/tmp/test-mycelium");
    return new Mycelium(props["dbname"], dbpath.c_str(), props);
  } else if (props["dbname"] == "writetwice") {
    std::string dbpath = props.GetProperty("dbpath","/tmp/test-writetwice");
    return new Writetwice(props["dbname"], dbpath.c_str(), props);
  } else if (props["dbname"] == "rocksdb_column_strawman") {
    std::string dbpath = props.GetProperty("dbpath","/tmp/test-rocksdb-strawman");
    return new RocksdbColumnStrawman(props["dbname"], dbpath.c_str(), props);
  } else if (props["dbname"] == "splitfirst") {
    std::string dbpath = props.GetProperty("dbpath","/tmp/test-splitfirst");
    return new Splitfirst(props["dbname"], dbpath.c_str(), props);
  } else return nullptr;
}

}
