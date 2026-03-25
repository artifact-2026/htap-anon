#include "db_helper.h"
#include <rocksdb/table.h>
#include <rocksdb/filter_policy.h>

using namespace std;

namespace ycsbc {

    mycelium::InputOutputDataType DBHelper::mapStringToDataType(const std::string& dataType) {
        if (dataType == "json" || dataType == "JSON") return mycelium::InputOutputDataType::JSON;
        if (dataType == "protobuf" || dataType == "PROTOBUF") return mycelium::InputOutputDataType::PROTOBUF;
        if (dataType == "flatbuffers" || dataType == "FLATBUFFERS") return mycelium::InputOutputDataType::FLATBUFFERS;
        return mycelium::InputOutputDataType::UNKNOWN;
    }

    void DBHelper::SetOptions(rocksdb::Options& options_, bool logging, utils::Properties &props)
    {
        int levels = utils::StrToInt(props.GetProperty("levels", "7"));
        int fieldcount = utils::StrToInt(props.GetProperty("fieldcount", "1"));
        
        if (!logging) {
            options_.info_log_level = rocksdb::InfoLogLevel::FATAL_LEVEL;
        }

        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;
        options_.max_open_files = -1;
        options_.compaction_style = rocksdb::kCompactionStyleLevel;
        options_.disable_auto_compactions = false;

        // Background thread pool: covers both compaction and flush.
        // 8 is a good default for NVMe servers; increase if compaction lags.
        options_.max_background_jobs = 8;
        // Allow up to 4 parallel sub-compactions per compaction job on NVMe.
        options_.max_subcompactions = 4;

        options_.num_levels = levels;
        options_.num_columns = fieldcount;

        // 64 MB memtable (RocksDB default); keep 4 in memory before stalling.
        options_.write_buffer_size = 64 * 1024 * 1024;
        options_.max_write_buffer_number = 4;
        //options_.level0_file_num_compaction_trigger = 4;   // default; compaction at 4×64MB=256MB L0
        //options_.level0_slowdown_writes_trigger = 20;      // default; throttle at 20×64MB=1.28GB L0
        //options_.level0_stop_writes_trigger = 36;          // default; hard stop at 36×64MB=2.3GB L0
        //options_.IncreaseParallelism(24);
        options_.use_direct_reads = true;
        options_.use_direct_io_for_flush_and_compaction = true;
        //options_.max_bytes_for_level_base = 256 * 1024 * 1024;  // default; = trigger × write_buffer_size
        //options_.target_file_size_base = 64 * 1024 * 1024;       // default; SST file size at L1
        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_cache = rocksdb::NewLRUCache(512 * 1024 * 1024);
        table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
        options_.table_factory = std::shared_ptr<rocksdb::TableFactory>(rocksdb::NewBlockBasedTableFactory(table_options));
        
    }

}
