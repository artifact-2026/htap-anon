#include "core/core_workload.h"
#include "test_rocks_db.h"
#include "lib/coding.h"

using namespace std;

namespace ycsbc {
    TestRocksDB::TestRocksDB(const char *dbfilename, utils::Properties &props) {
        noResults = 0;
        SetOptions(props);
        rocksdb::Status s = rocksdb::DB::Open(options_, 
                                              dbfilename,
                                              &rocksdb_);
    }

    int TestRocksDB::Read(const std::string &table, const std::string &key, const std::vector<std::string> *fields,
                      data::Row &result) 
    {
        std::string value;
        rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(), key, &value);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    int TestRocksDB::Scan(const std::string &table, const std::string &begin_key,
                          int32_t len, const std::vector<std::string> *fields,
                          std::vector<data::Row> &result) 
    {
        result.clear();
        auto it = rocksdb_->NewIterator(rocksdb::ReadOptions());
        it->Seek(begin_key);
        for (int i = 0; i < len && it->Valid(); i++) {
            std::string value = it->value().ToString();
            data::Row row;
            row.ParseFromString(value);
            result.push_back(row);
            it->Next();
        }
        
        return result.size();
    }

    int TestRocksDB::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        rocksdb::Status s = rocksdb_->Put(rocksdb::WriteOptions(), key, values);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    int TestRocksDB::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int TestRocksDB::Delete(const std::string &table, const std::string &key)
    {
        rocksdb::Status s = rocksdb_->Delete(rocksdb::WriteOptions(), key);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    void TestRocksDB::SetOptions(utils::Properties &props) {

        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;

        options_.max_background_jobs = 16;
        options_.max_write_buffer_number = 32;
        options_.target_file_size_base = 64ul * 1024 * 1024;
        options_.write_buffer_size = 2 << 30;
        options_.db_write_buffer_size = 2 << 30;

        options_.level0_file_num_compaction_trigger = 8;
        options_.level0_slowdown_writes_trigger = 16;     
        options_.level0_stop_writes_trigger = 16;

        options_.use_direct_reads = true;
        options_.use_direct_io_for_flush_and_compaction = true;

        options_.max_open_files = 20480;
        options_.max_file_opening_threads = 32;

        uint64_t nums = stoi(props.GetProperty(CoreWorkload::RECORD_COUNT_PROPERTY));
        uint32_t key_len = stoi(props.GetProperty(CoreWorkload::KEY_LENGTH));
        uint32_t value_len = stoi(props.GetProperty(CoreWorkload::FIELD_LENGTH_PROPERTY));
        uint32_t cache_size = nums * (key_len + value_len) / 10;
        if(cache_size < 8 << 20) {
            cache_size = 8 << 20;
        }
        cache_ = rocksdb::NewLRUCache(cache_size);

        bool statistics = utils::StrToBool(props["dbstatistics"]);
        if(statistics){
            dbstats_ = rocksdb::CreateDBStatistics();
            options_.statistics = dbstats_;
        }
    }

    void TestRocksDB::SerializeValue(std::vector<KVPair> &kvs, std::string &value) {

    }

    void TestRocksDB::DeSerializeValue(std::string &value,
                          const std::vector<std::string> *fields,
                          std::vector<KVPair> &kvs) 
    {

    }
}
