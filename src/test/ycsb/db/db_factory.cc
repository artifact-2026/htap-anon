#include "db/db_factory.h"

#include <string>
#include <iostream>
#include "db/test_rocks_db.h"
#include "db/test_cracking.h"
#include "db/test_indexing.h"
#include "db/test_precracking.h"
#include "db/splitfirst.h"
#include "db/test_flatbuffers.h"
#include "db/test_fb_cracker.h"

using namespace std;

namespace ycsbc {
//using ycsbc::DB;
//using ycsbc::DBFactory;

DB* DBFactory::CreateDB(utils::Properties &props) {
  if (props["dbname"] == "baseline") {
    std::string dbpath = props.GetProperty("dbpath","/tmp/test-baseline");
    return new TestRocksDB(dbpath.c_str(), props);
  } else if (props["dbname"] == "cracking") {
    std::string dbpath = props.GetProperty("dbpath","/tmp/test-cracking");
    return new Mycelium(props["dbname"], dbpath.c_str(), props);
  } else if (props["dbname"] == "indexing") {
    std::string dbpath = props.GetProperty("dbpath","/tmp/test-indexing");
    return new Indexing(props["dbname"], dbpath.c_str(), props);
  } else if (props["dbname"] == "precracking") {
    std::string dbpath = props.GetProperty("dbpath","/tmp/test-precracking");
    return new RocksdbColumnStrawman(props["dbname"], dbpath.c_str(), props);
  } else if (props["dbname"] == "flatbuffers") {
    std::string dbpath = props.GetProperty("dbpath","/tmp/test-fb");
    return new TestFlatBuffers(props["dbname"], dbpath.c_str(), props);
  } else if (props["dbname"] == "crackfb") {
    std::string dbpath = props.GetProperty("dbpath","/tmp/test-crackfb");
    return new TestFBCracker(props["dbname"], dbpath.c_str(), props);
  } else return nullptr;
}

}
