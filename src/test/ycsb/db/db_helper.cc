#include "db_helper.h"
#include <rocksdb/table.h>
#include <rocksdb/filter_policy.h>

using namespace std;

namespace ycsbc {

    rocksdb::InputOutputDataType DBHelper::mapStringToDataType(const std::string& dataType) {
        if (dataType == "json" || dataType == "JSON") return rocksdb::InputOutputDataType::JSON;
        if (dataType == "protobuf" || dataType == "PROTOBUF") return rocksdb::InputOutputDataType::PROTOBUF;
        if (dataType == "flatbuffers" || dataType == "FLATBUFFERS") return rocksdb::InputOutputDataType::FLATBUFFERS;
        return rocksdb::InputOutputDataType::UNKNOWN;
    }

    void DBHelper::SetOptions(rocksdb::Options& options_, const char *dbfilename, bool logging, int levels, int fieldcount)
    {
        if (!logging) {
            options_.info_log_level = rocksdb::InfoLogLevel::FATAL_LEVEL;
        }

        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;
        options_.max_open_files = -1;
        options_.compaction_style = rocksdb::kCompactionStyleLevel;
        options_.disable_auto_compactions = false;

	    options_.env->SetBackgroundThreads(20, rocksdb::Env::Priority::LOW);
        options_.env->SetBackgroundThreads(8, rocksdb::Env::Priority::HIGH);
        options_.max_background_compactions = 20;
        options_.max_background_flushes = 8;

        options_.max_subcompactions = 16;

        options_.num_levels = levels;
        options_.num_columns = fieldcount;
        options_.SetTransformerType(rocksdb::TransformerType::DISTRIBUTOR);

        options_.write_buffer_size = 128 * 1024 * 1024;
        options_.max_write_buffer_number = 8;
        options_.level0_slowdown_writes_trigger = 50;
        options_.level0_stop_writes_trigger = 80;
        options_.IncreaseParallelism(24);
        options_.use_direct_reads = true;
        options_.use_direct_io_for_flush_and_compaction = true;
	    options_.max_bytes_for_level_base = 256 * 1024 * 1024;

        options_.target_file_size_base = 256 * 1024 * 1024;
        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_cache = rocksdb::NewLRUCache(512 * 1024 * 1024);
        table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
        options_.table_factory = std::shared_ptr<rocksdb::TableFactory>(rocksdb::NewBlockBasedTableFactory(table_options));
        
    }

}
