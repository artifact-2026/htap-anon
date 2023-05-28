#ifndef CABINDB_CABINDB_H
#define CABINDB_CABINDB_H

#include <string>
#include <vector>
#include "cabindb_namespace.h"
#include "compactor.h"
#include <rocksdb/options.h>
#include "column_family_util.h"

namespace rocksdb{
  class DB;
  class Env;
  class Cache;
  class FilterPolicy;
  class Snapshot;
  class Slice;
  class WriteBatch;
  class Iterator;
  class Logger;
  class ColumnFamilyDescriptor;
  class ColumnFamilyHandle;
  class Status;
  struct Options;
  struct BlockBasedTableOptions;
  struct DBOptions;
  struct ColumnFamilyOptions;
}

namespace CABINDB_NAMESPACE {

const int kNumOfLevels = 4;

enum Status {
  kOK = 0,
  kError,
  kNotFound,
  kNotImplemented
};

typedef std::pair<std::string, std::string> KVPair;

class CabinDB {
  public:
    CabinDB(const char *dbfilename, rocksdb::Options& options, int field_count, bool bootstrap);

    Status Read(const std::string &table, const std::string &key, std::string &value);

    Status Scan(const std::string &table, const std::string &key, int len, std::vector<std::string> &values);

    Status Insert(const std::string &table, const std::string &key, std::string &value);

    Status Delete(const std::string &table, const std::string &key);

    ~CabinDB();

  private:
    rocksdb::DB *db_;
    std::string dbpath_;
    rocksdb::Options options_;
    std::vector<std::vector<std::string> > leveled_cf_names_;
    std::vector<rocksdb::ColumnFamilyDescriptor> cf_descriptors_;
    std::vector<rocksdb::ColumnFamilyHandle*> cfhandles_;
};

}

#endif
