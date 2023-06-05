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

namespace CABINDB_NAMESPACE {
struct CompactionTask;

class Compactor : public EventListener {
 public:
    virtual CompactionTask* PickCompaction(DB* db,
                                           const std::string& cf_name,
                                           const std::string& output_cf_name,
                                           int level) = 0;
    // Schedule and run the specified compaction task in background.
    virtual void ScheduleCompaction(CompactionTask* task) = 0;
};

struct CompactionTask {
  CompactionTask(DB* _db, Compactor* _compactor,
                 const std::string& _column_family_name,
                 const std::vector<std::string>& _input_file_names,
                 const int _compact_level,
		 const int _output_level,
		 const std::string& _output_column_family_name,
                 rocksdb::ColumnFamilyHandle* _output_cf_handle,
                 const CompactionOptions& _compact_options, bool _retry_on_fail)
      : db(_db),
        compactor(_compactor),
        column_family_name(_column_family_name),
        input_file_names(_input_file_names),
        compact_level(_compact_level),
	output_level(_output_level),
	output_column_family_name(_output_column_family_name),
        output_cf_handle(_output_cf_handle),
        compact_options(_compact_options),
        retry_on_fail(_retry_on_fail) {}
  DB* db;
  Compactor* compactor;
  const std::string& column_family_name;
  std::vector<std::string> input_file_names;
  int compact_level;
  int output_level;
  const std::string& output_column_family_name;
  rocksdb::ColumnFamilyHandle* output_cf_handle;
  CompactionOptions compact_options;
  bool retry_on_fail;
};

class CabinCompactor : public Compactor {
 public:
    CabinCompactor(const Options &options, int num_levels,
                        const std::vector<std::vector<std::string> > &leveled_cf_names,
                        std::map<std::string, rocksdb::ColumnFamilyHandle*> &cf_handles): 
                        options_(options), num_levels_(num_levels),
			leveled_cf_names_(leveled_cf_names), cf_handles_(cf_handles) {
      compact_options_.compression = options_.compression;
      compact_options_.output_file_size_limit = options_.target_file_size_base;
    };

    void OnFlushCompleted(DB* db, const FlushJobInfo& info) override {
      std::vector<CompactionTask*> tasks;
      std::lock_guard<std::mutex> lock(_mutex);
      int level = 0;
      for (auto cfnames : leveled_cf_names_) {
         for (auto cfname : cfnames) {
            for (auto next_level_cf_name : next_level_dest_[cfname]) {
               CompactionTask* task = PickCompaction(db, cfname,
                                          next_level_cf_name, level);
               if (task != nullptr) {
                  if (info.triggered_writes_stop) {
                     task->retry_on_fail = true;
                  }
                  // Schedule compaction in a different thread.
                  ScheduleCompaction(task);
               }
            }
         }
	 level++;
      }
    }

    CompactionTask* PickCompaction(DB* db, const std::string& cf_name,
                                   const std::string& output_cf_name,
                                   int compact_level) override {
      rocksdb::ColumnFamilyMetaData cf_meta;
      db->GetColumnFamilyMetaData(cf_handles_[cf_name], &cf_meta);

      std::vector<std::string> input_file_names;
      for (auto level : cf_meta.levels) {
         if (level.level != compact_level) {
            continue;
         }
	      for (auto file : level.files) {
            if (file.being_compacted) {
               return nullptr;
            }
            input_file_names.push_back(file.name);
         }
      }

      int output_level = compact_level + 1;
      if (uint64_t(compact_level) == leveled_cf_names_.size() - 1) {
         output_level = compact_level;
      }

      rocksdb::ColumnFamilyHandle* output_cf_handle = cf_handles_[output_cf_name];
      return new CompactionTask(db, this, cf_name, input_file_names,
		     compact_level, output_level, output_cf_name,
                     output_cf_handle, compact_options_, false);
    }

    void ScheduleCompaction(CompactionTask* task) override {
      options_.env->Schedule(&CabinCompactor::CompactFiles, task);
    }
    
    static void CompactFiles(void* arg) {
      std::unique_ptr<CompactionTask> task(
        reinterpret_cast<CompactionTask*>(arg));
      assert(task);
      assert(task->db);
      Status s = task->db->CompactFiles(task->compact_options,
                                          task->output_cf_handle,
                                          task->input_file_names,
                                          task->output_level);
      printf("CompactFiles() finished with status %s\n", s.ToString().c_str());
      if (!s.ok() && !s.IsIOError() && task->retry_on_fail) {
         // If a compaction task with its retry_on_fail=true failed,
         // try to schedule another compaction in case the reason
         // is not an IO error.
         CompactionTask* new_task =
		 task->compactor->PickCompaction(task->db, task->column_family_name,
                     task->output_column_family_name, task->compact_level);
         task->compactor->ScheduleCompaction(new_task);
      }
    }

    ~CabinCompactor() {};
 private:
    Options options_;
    CompactionOptions compact_options_;
    int num_levels_;
    const std::vector<std::vector<std::string> > leveled_cf_names_; // col fams on each level
    std::map<std::string, rocksdb::ColumnFamilyHandle*> cf_handles_;
    std::map<std::string, std::vector<std::string> > next_level_dest_;
    std::mutex _mutex;
};

}

#endif
