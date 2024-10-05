#include "core/core_workload.h"
#include "test_rocks_db.h"
#include "lib/coding.h"

using namespace std;

namespace ycsbc {
    TestRocksDB::TestRocksDB(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        noResults = 0;
        int levels = utils::StrToInt(props.GetProperty("levels", "6"));
        int fieldcount = utils::StrToInt(props.GetProperty("fieldcount", "1"));
        SetOptions(props, bootstrap, levels, fieldcount);

        std::vector<rocksdb::ColumnFamilyDescriptor> column_family_descriptors;
        GetColumnFamilyDescriptors(dbname, column_family_descriptors);
        std::vector<rocksdb::ColumnFamilyHandle*> cf_handles;

        if (bootstrap) {
            rocksdb::Status s = rocksdb::DB::Open(options_, 
                                              dbfilename,
                                              &rocksdb_);

            if (!s.ok()){
                std::cerr<<"Can't open rocksdb "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }

            s = rocksdb_->CreateColumnFamilies(column_family_descriptors, &cf_handles);
        } else {
            column_family_descriptors.push_back(rocksdb::ColumnFamilyDescriptor(
                    rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions(options_)));
            rocksdb::Status s = rocksdb::DB::Open(options_,
                                              dbfilename,
                                              column_family_descriptors,
                                              &cf_handles,
                                              &rocksdb_);
            if (!s.ok()){
                std::cerr<<"Can't open rocksdb "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }
        }
        BuildColumnFamilyHandles(column_family_descriptors, cf_handles);
    }

    void TestRocksDB::GetColumnFamilyDescriptors(const std::string &dbname, std::vector<rocksdb::ColumnFamilyDescriptor>& column_families)
    {
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(
                dbname, rocksdb::ColumnFamilyOptions(options_)));
    }

    /*
    * Read is for point query over all columns
    */
    int TestRocksDB::Read(const std::string &table, const std::string &key, const std::set<std::string> *fields,
                      const std::string &req_dist, bool index_access, std::string &result) 
    {
        rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandle_, key, &result);
        if (s.ok()) {    
            return 0;
        }
        return 1;
    }

    int TestRocksDB::Scan(const std::string &table, const std::string &begin_key,
                          const std::string &end_key, const std::set<std::string> *fields,
                          std::vector<std::string> &result) 
    {
        result.clear();
        auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandle_);
        it->Seek(begin_key);
        while (it->Valid()) {
            if (it->key().ToString() < end_key) {
                result.push_back(it->value().ToString());
            } else {
                break;
            }
            it->Next();
        }
        return result.size();
    }

    int TestRocksDB::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        rocksdb::Status s = rocksdb_->Put(rocksdb::WriteOptions(), cfhandle_, key, values);
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
        rocksdb::Status s = rocksdb_->Delete(rocksdb::WriteOptions(), cfhandle_, key);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    void TestRocksDB::SetOptions(utils::Properties &props, bool logging, int levels, int fieldcount)
    {
        if (!logging) {
            options_.info_log_level = rocksdb::InfoLogLevel::FATAL_LEVEL;
        }
        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;

        options_.num_levels = levels;
        options_.num_columns = fieldcount;
        options_.SetTransformerType(rocksdb::TransformerType::NOTRANSFORMATION);

        options_.IncreaseParallelism(16);
        options_.level0_slowdown_writes_trigger = 16;     
        options_.level0_stop_writes_trigger = 24;
        options_.max_open_files = -1;
        options_.level0_file_num_compaction_trigger = 8;

        options_.max_write_buffer_number = 3;
        options_.write_buffer_size = 67108864;
        options_.target_file_size_base = 67108864;

        options_.use_direct_reads = true;
        options_.use_direct_io_for_flush_and_compaction = true;

        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_cache = nullptr;  // Disable the block cache
        options_.table_factory = std::shared_ptr<rocksdb::TableFactory>(
            rocksdb::NewBlockBasedTableFactory(table_options));
    }

    void TestRocksDB::BuildColumnFamilyHandles(std::vector<rocksdb::ColumnFamilyDescriptor> &column_family_descriptors,
                                                std::vector<rocksdb::ColumnFamilyHandle *> handles)
    {
        for (size_t i = 0; i < handles.size(); i++) {
            if (column_family_descriptors[i].name != rocksdb::kDefaultColumnFamilyName) {
                cfhandle_ = handles[i];
            }
        }
    }
}
