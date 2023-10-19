#include <iostream>
#include "compactor.h"

namespace ROCKSDB_NAMESPACE {

CabinCompactor::CabinCompactor(const Options &options) : options_(options)
{
    compact_options_.compression = options_.compression;
    compact_options_.output_file_size_limit = options_.target_file_size_base;
};

void CabinCompactor::SetColumnFamilyHandles(std::map<std::string, rocksdb::ColumnFamilyHandle*>& cfhandles)
{
    cf_handles_.insert(cfhandles.begin(), cfhandles.end());
}

void CabinCompactor::OnFlushCompleted(DB* db, const FlushJobInfo& info) {
    CompactionTask* task = PickCompaction(db, info.cf_name, 0);
    if (task != nullptr) {
        if (info.triggered_writes_stop) {
            task->retry_on_fail = true;
        }
        ScheduleCompaction(task);
    }

    int splits = 1;
    for (int i = 1; i < options_.compacting_column_family_num_levels; i++) {
        splits *= 2;
        if (i == options_.compacting_column_family_num_levels-1 || i > options_.num_columns) {
            splits = options_.num_columns;
        }
        for (int j = 0; j < splits; j++) {
            std::string column_family_name = info.cf_name + "_sys_cf_" + std::to_string(i) + "_" + std::to_string(j);
            task = PickCompaction(db, column_family_name, 1);
            ScheduleCompaction(task);
        }
    }
}

CompactionTask* CabinCompactor::PickCompaction(DB* db, const std::string& cf_name, const int input_level) {
    rocksdb::ColumnFamilyMetaData cf_meta;
    db->GetColumnFamilyMetaData(cf_handles_.at(cf_name), &cf_meta);

    std::vector<std::string> input_file_names;
    for (auto level : cf_meta.levels) {
	    for (auto file : level.files) {
            if (file.being_compacted) {
               return nullptr;
            }
            input_file_names.push_back(file.name);
        }
    }

    return new CompactionTask(db, this, cf_name, cf_handles_.at(cf_name), input_file_names, input_level, compact_options_, false);
}

void CabinCompactor::ScheduleCompaction(CompactionTask* task) {
    if (task->column_family_name.find("sys") == std::string::npos) {
        printf("Scheduling compaction task for column family: %s\n", task->column_family_name.c_str());
        options_.env->Schedule(&CabinCompactor::CompactFiles, task);
    }
}
    
void CabinCompactor::CompactFiles(void* arg) {
    std::unique_ptr<CompactionTask> task(reinterpret_cast<CompactionTask*>(arg));
    assert(task);
    assert(task->db);
    printf("compacting for column family: %s\n", task->column_family_name.c_str());
    Status s = task->db->CompactFiles(task->compact_options,
                                      task->column_family_handle,
                                      task->input_file_names,
                                      task->output_level);
    if (s.ok()) {
        printf("CompactFiles() finished with status %s\n", s.ToString().c_str());
    } else if (!s.IsIOError() && task->retry_on_fail) {
         // If a compaction task with its retry_on_fail=true failed,
         // try to schedule another compaction in case the reason
         // is not an IO error.
         CompactionTask* new_task =

		 task->compactor->PickCompaction(task->db, task->column_family_name, 0);
         task->compactor->ScheduleCompaction(new_task);
    }
}

}