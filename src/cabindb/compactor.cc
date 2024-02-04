#include <iostream>
#include "compactor.h"

namespace ROCKSDB_NAMESPACE {

std::map<rocksdb::ColumnFamilyHandle*, std::vector<std::string> > CabinCompactor::compact_files_map_;

CabinCompactor::CabinCompactor(const Options &options) 
    : options_(options)
{
    compact_options_.compression = options_.compression;
    compact_options_.output_file_size_limit = options_.target_file_size_base;
};

void CabinCompactor::SetColumnFamilyHandles(std::map<std::string, rocksdb::ColumnFamilyHandle*>& cfhandles)
{
    cf_handles_.insert(cfhandles.begin(), cfhandles.end());
}

void CabinCompactor::OnFlushCompleted(DB* db, const FlushJobInfo& info) {
    CompactionTask* task = PickCompaction(db, info.cf_name);
    if (task != nullptr) {
        if (info.triggered_writes_stop) {
            task->retry_on_fail = true;
        }
        ScheduleCompaction(task);

        for (auto cf : cf_handles_) {
            task = PickCompaction(db, cf.first);
            if (task != nullptr) {
                if (info.triggered_writes_stop) {
                    task->retry_on_fail = true;
                }
                ScheduleCompaction(task);
            }
        }
    }
}

CompactionTask* CabinCompactor::PickCompaction(DB* db, const std::string& cf_name) {
    rocksdb::ColumnFamilyHandle* cfhandle = cf_handles_[cf_name];
    if (cfhandle == nullptr) {
        return nullptr;
    }
    if (compact_files_map_.find(cfhandle) != compact_files_map_.end()) {
        return nullptr;
    }
    rocksdb::ColumnFamilyMetaData cf_meta;
    db->GetColumnFamilyMetaData(cfhandle, &cf_meta);

    std::vector<std::string> input_file_names;
    for (auto level : cf_meta.levels) {
	    for (auto file : level.files) {
            if (file.being_compacted) {
               return nullptr;
            }
            input_file_names.push_back(file.name);
        }
    }

    if (input_file_names.size() < 4) {
        return nullptr;
    }
    
    compact_files_map_.insert(std::pair<rocksdb::ColumnFamilyHandle*, std::vector<std::string> >(cf_handles_.at(cf_name), input_file_names));
    return new CompactionTask(db, this, cf_name, cfhandle, input_file_names, options_.num_levels-1, compact_options_, false);
}

void CabinCompactor::ScheduleCompaction(CompactionTask* task) {
    options_.env->Schedule(&CabinCompactor::CompactFiles, task);
}
    
void CabinCompactor::CompactFiles(void* arg) {
    std::unique_ptr<CompactionTask> task(reinterpret_cast<CompactionTask*>(arg));
    assert(task);
    assert(task->db);
    std::cout << "Compacting: " << task->column_family_name << std::endl;
    Status s = task->db->CompactFiles(task->compact_options,
                                      task->column_family_handle,
                                      task->input_file_names,
                                      task->output_level);

    if (s.ok()) {
        printf("CompactFiles finished with status %s\n", s.ToString().c_str());
        compact_files_map_.erase(task->column_family_handle);
    } else if (!s.IsIOError() && task->retry_on_fail) {
         // If a compaction task with its retry_on_fail=true failed,
         // try to schedule another compaction in case the reason
         // is not an IO error.
         CompactionTask* new_task = task->compactor->PickCompaction(task->db, task->column_family_name);
         task->compactor->ScheduleCompaction(new_task);
    }
}

}