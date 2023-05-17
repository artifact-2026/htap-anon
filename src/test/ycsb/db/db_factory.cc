#include "db/db_factory.h"

#include <string>
#include <iostream>
#include "db/test_cabin_db.h"

using namespace std;

namespace ycsbc {
//using ycsbc::DB;
//using ycsbc::DBFactory;

DB* DBFactory::CreateDB(utils::Properties &props) {
  if (props["dbname"] == "cabindb") {
    std::string dbpath = props.GetProperty("dbpath","/tmp/test-cabindb");
    return new CabinDB(dbpath.c_str(), props);
  } else return nullptr;
}

}
