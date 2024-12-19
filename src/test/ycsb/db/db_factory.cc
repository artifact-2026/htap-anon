#include "db/db_factory.h"

#include <string>
#include <iostream>
#include "db/test_rocks_db.h"
#include "db/test_cracking.h"
#include "db/test_indexing.h"
#include "db/test_precracking.h"
#include "db/test_flatbuffers.h"
#include "db/test_fb_cracker.h"
#include "db/test_preconverting.h"
#include "db/test_preindexing.h"
#include "db/test_crackplus.h"
#include "db/test_mynoop.h"

using namespace std;

namespace ycsbc {
//using ycsbc::DB;
//using ycsbc::DBFactory;

DB* DBFactory::CreateDB(utils::Properties &props) {
  if (props["dbname"] == "baseline") {
    std::string dbpath = props.GetProperty("dbpath","/tmp/test-baseline");
    return new TestRocksDB(props["dbname"], dbpath.c_str(), props);
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
    return new TestFbCracker(props["dbname"], dbpath.c_str(), props);
  } else if (props["dbname"] == "preconverting") {
    std::string dbpath = props.GetProperty("dbpath","/tmp/test-preconverting");
    return new TestPreconverting(props["dbname"], dbpath.c_str(), props);
  } else if (props["dbname"] == "preindexing") {
    std::string dbpath = props.GetProperty("dbpath","/tmp/test-preindexing");
    return new TestPreindexing(props["dbname"], dbpath.c_str(), props);
  } else if (props["dbname"] == "crackplus") {
    std::string dbpath = props.GetProperty("dbpath","/tmp/test-crackplus");
    return new MyceliumWriteBoth(props["dbname"], dbpath.c_str(), props);
  } else if (props["dbname"] == "mynoop") {
    std::string dbpath = props.GetProperty("dbpath","/tmp/test-mynoop");
    return new TestMynoop(props["dbname"], dbpath.c_str(), props);
  } else return nullptr;
}

}
