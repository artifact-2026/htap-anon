#include "db/db_factory.h"

#include <string>
#include <iostream>
#include "db/test_rocks_db.h"
#include "db/mycelium.h"
#include "db/mycelium_row_strawman.h"
#include "db/mycelium_column_strawman.h"

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
  } else if (props["dbname"] == "mycelium_row_strawman") {
    std::string dbpath = props.GetProperty("dbpath","/tmp/test-mycelium");
    return new MyceliumRowStrawman(dbpath.c_str(), props);
  } else if (props["dbname"] == "mycelium_column_strawman") {
    std::string dbpath = props.GetProperty("dbpath","/tmp/test-mycelium");
    return new MyceliumColumnStrawman(dbpath.c_str(), props);
  } else return nullptr;
}

}
