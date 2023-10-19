#ifndef CABINDB_COMPACTOR_H
#define CABINDB_COMPACTOR_H

#include <mutex>
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/options.h"

using rocksdb::ColumnFamilyMetaData;
using rocksdb::ColumnFamilyHandle;
using rocksdb::CompactionOptions;
using rocksdb::DB;
using rocksdb::EventListener;
using rocksdb::FlushJobInfo;
using rocksdb::Options;
using rocksdb::ReadOptions;
using rocksdb::Status;
using rocksdb::WriteOptions;

namespace ROCKSDB_NAMESPACE {
struct CompactionTask;

class Compactor : public EventListener {
 public:
    virtual CompactionTask* PickCompaction(DB* db,
                                           const std::string& cf_name,
                                           const int input_level) = 0;
    // Schedule and run the specified compaction task in background.
    virtual void ScheduleCompaction(CompactionTask* task) = 0;
};

struct CompactionTask {
  CompactionTask(DB* _db, Compactor* _compactor,
                 const std::string& _column_family_name,
                 ColumnFamilyHandle* _column_family_handle,
                 const std::vector<std::string>& _input_file_names,
		           const int _output_level,
                 const CompactionOptions& _compact_options, bool _retry_on_fail)
      : db(_db),
        compactor(_compactor),
        column_family_name(_column_family_name),
        column_family_handle(_column_family_handle),
        input_file_names(_input_file_names),
	     output_level(_output_level),
        compact_options(_compact_options),
        retry_on_fail(_retry_on_fail) {}
  DB* db;
  Compactor* compactor;
  const std::string& column_family_name;
  ColumnFamilyHandle* column_family_handle;
  std::vector<std::string> input_file_names;
  int output_level;
  CompactionOptions compact_options;
  bool retry_on_fail;
};

class CabinCompactor : public Compactor {
 public:
    CabinCompactor(const Options &options);

    void OnFlushCompleted(DB* db, const FlushJobInfo& info) override;

    CompactionTask* PickCompaction(DB* db, const std::string& cf_name, const int input_level) override;

    void ScheduleCompaction(CompactionTask* task) override;
    
    static void CompactFiles(void* arg);

    void SetColumnFamilyHandles(std::map<std::string, rocksdb::ColumnFamilyHandle*>& cfhandles);

    ~CabinCompactor() {};
 private:
    Options options_;
    CompactionOptions compact_options_;
    std::map<std::string, rocksdb::ColumnFamilyHandle*> cf_handles_;
    std::mutex _mutex;
};

}

#endif
