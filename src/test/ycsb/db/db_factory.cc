#include "db/db_factory.h"

#include <string>
#include <iostream>
#include "db/test_cabin_db.h"
#include "db/test_rocks_db.h"

using namespace std;

namespace ycsbc {
//using ycsbc::DB;
//using ycsbc::DBFactory;

DB* DBFactory::CreateDB(utils::Properties &props) {
  if (props["dbname"] == "cabindb") {
    std::string dbpath = props.GetProperty("dbpath","/tmp/test-cabindb");
    return new TestCabinDB(dbpath.c_str(), props);
  } else if (props["dbname"] == "rocksdb") {
    std::string dbpath = props.GetProperty("dbpath","/tmp/test-rocksdb");
    return new TestRocksDB(dbpath.c_str(), props);
  } else return nullptr;
}

}
