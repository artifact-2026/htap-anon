#include "compactor.h"

namespace ROCKSDB_NAMESPACE {

CabinCompactor::CabinCompactor(const Options &options) : options_(options)
{
    compact_options_.compression = options_.compression;
    compact_options_.output_file_size_limit = options_.target_file_size_base;
};

CabinCompactor::CabinCompactor(const Options &options,
                        const std::vector<std::vector<std::string> > &leveled_cf_names,
                        std::map<std::string, rocksdb::ColumnFamilyHandle*> &cf_handles)
                        : options_(options), 
			              leveled_cf_names_(leveled_cf_names), 
                          cf_handles_(cf_handles)
{
    compact_options_.compression = options_.compression;
    compact_options_.output_file_size_limit = options_.target_file_size_base;
}

void CabinCompactor::OnFlushCompleted(DB* db, const FlushJobInfo& info) {
    CompactionTask* task = PickCompaction(db, info.cf_name);
    if (task != nullptr) {
        if (info.triggered_writes_stop) {
            task->retry_on_fail = true;
        }

        ScheduleCompaction(task);
    }
    /*std::vector<CompactionTask*> tasks;
    std::lock_guard<std::mutex> lock(_mutex);
    int level = 0;
    for (auto cfnames : leveled_cf_names_) {
        for (auto cfname : cfnames) {
            for (auto next_level_cf_name : next_level_dest_[cfname]) {
               CompactionTask* task = PickCompaction(db, cfname, next_level_cf_name, level);
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
    }*/
}

CompactionTask* CabinCompactor::PickCompaction(DB* db, const std::string& cf_name) {
    rocksdb::ColumnFamilyMetaData cf_meta;
    db->GetColumnFamilyMetaData(db->DefaultColumnFamily(), &cf_meta);

    std::vector<std::string> input_file_names;
    for (auto level : cf_meta.levels) {
        /*if (level.level != compact_level) {
            continue;
        }*/
	    for (auto file : level.files) {
            if (file.being_compacted) {
               return nullptr;
            }
            input_file_names.push_back(file.name);
        }
    }

    /*int output_level = compact_level + 1;
    if (uint64_t(compact_level) == leveled_cf_names_.size() - 1) {
        output_level = compact_level;
    }*/

    //rocksdb::ColumnFamilyHandle* output_cf_handle = cf_handles_[output_cf_name];
    return new CompactionTask(db, this, cf_name, input_file_names,
                        options_.num_levels - 1, compact_options_, false);
}

void CabinCompactor::ScheduleCompaction(CompactionTask* task) {
    options_.env->Schedule(&CabinCompactor::CompactFiles, task);
}
    
void CabinCompactor::CompactFiles(void* arg) {
    std::unique_ptr<CompactionTask> task(reinterpret_cast<CompactionTask*>(arg));
    assert(task);
    assert(task->db);
    Status s = task->db->CompactFiles(task->compact_options,
                                      task->input_file_names,
                                      task->output_level);
    printf("CompactFiles() finished with status %s\n", s.ToString().c_str());
    if (!s.ok() && !s.IsIOError() && task->retry_on_fail) {
         // If a compaction task with its retry_on_fail=true failed,
         // try to schedule another compaction in case the reason
         // is not an IO error.
         CompactionTask* new_task =
		 task->compactor->PickCompaction(task->db, task->column_family_name);
         task->compactor->ScheduleCompaction(new_task);
    }
}

}