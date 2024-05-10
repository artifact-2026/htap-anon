#ifndef CABINDB_CABINDB_H
#define CABINDB_CABINDB_H

#include <string>
#include <vector>
#include "cabindb_namespace.h"
#include "compactor.h"
#include <rocksdb/options.h>
#include "column_family_util.h"
#include "proto/columns.pb.h"

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
  class Cracker;
  struct Options;
  struct BlockBasedTableOptions;
  struct DBOptions;
  struct ColumnFamilyOptions;
}

namespace ROCKSDB_NAMESPACE {

const int kNumOfLevels = 4;

typedef std::pair<std::string, std::string> KVPair;

class CabinDB {
  public:
    CabinDB(const std::string& dbname,
            const char *dbfilename,
            bool bootstrap,
            bool transform,
            std::string translevel);

    int Read(const std::string &table, const std::string &key,
                 const std::set<std::string> *fields,
                 std::string &result);

    int Scan(const std::string &table, const std::string &begin_key,
                 int32_t len, const std::set<std::string> *fields,
                 std::vector<std::string> &result);

    int Insert(const std::string &table, const std::string &key,
                 std::string &values);

    int Update(const std::string &table, const std::string &key,
                 std::string &values);

    int Delete(const std::string &table, const std::string &key);

    ~CabinDB();

  private:
    rocksdb::DB *rocksdb_;
    rocksdb::Options options_;
    std::map<std::string, rocksdb::ColumnFamilyHandle*> cfhandles_;
    std::vector<rocksdb::ColumnFamilyHandle*> leveled_cfhandles_;

    void SetOptions(const char *dbfilename);
	  void KeepOnlyRequestedFields(data::Row &row,
                const std::set<std::string> *fields, data::Row &selectedColumns);
    void GetColumnFamilyDescriptors(const std::string& dbname,
                                    std::vector<rocksdb::ColumnFamilyDescriptor>& column_families,
                                    std::string translevel);
    void BuildColumnFamilyHandleMap(std::vector<rocksdb::ColumnFamilyDescriptor>& column_family_descriptors,
                                    std::vector<rocksdb::ColumnFamilyHandle*> handles);
};

}

#endif
