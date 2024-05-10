#include "core/core_workload.h"
#include "test_rocks_db.h"
#include "lib/coding.h"
#include "cabindb/compactor.h"
#include "test_flatbuffers.h"
#include "lib/rocksdb/transformer/bytecoder.h"

using namespace std;

namespace ycsbc {
    TestFlatBuffers::TestFlatBuffers(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        noResults = 0;
        SetOptions(props);
        rocksdb::Status s = rocksdb::DB::Open(options_, 
                                              dbfilename,
                                              &rocksdb_);
    }

    /*
    * Read is for point query over all columns
    */
    int TestFlatBuffers::Read(const std::string &table, const std::string &key, const std::set<std::string> *fields,
                      std::string &result) 
    {
        rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(), key, &result);
        
        if (s.ok()) {
            //result.ParseFromString(value);
            return 0;
        }

        noResults++;
        return 1;
    }

    int TestFlatBuffers::Scan(const std::string &table, const std::string &begin_key,
                          int32_t len, const std::set<std::string> *fields,
                          std::vector<std::string> &result) 
    {
        result.clear();
        auto it = rocksdb_->NewIterator(rocksdb::ReadOptions());
        it->Seek(begin_key);
        for (int i = 0; i < len && it->Valid(); i++) {
            std::string value = it->value().ToString();

	        if (fields != NULL) {
                data::Row row;
                row.ParseFromString(value);
                data::Row selectedColumns;
                KeepOnlyRequestedFields(row, fields, selectedColumns);
                std::string stitchedValue;
                selectedColumns.SerializeToString(&stitchedValue);
                result.push_back(stitchedValue);
	        } else {
	            result.push_back(value);
            }	      
            it->Next();
        }
        
        return result.size();
    }

    int TestFlatBuffers::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        rocksdb::Status s = rocksdb_->Put(rocksdb::WriteOptions(), key, values);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    int TestFlatBuffers::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int TestFlatBuffers::Delete(const std::string &table, const std::string &key)
    {
        rocksdb::Status s = rocksdb_->Delete(rocksdb::WriteOptions(), key);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    void TestFlatBuffers::SetOptions(utils::Properties &props)
    {
        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;

        options_.IncreaseParallelism(16);
        options_.level0_slowdown_writes_trigger = 9999999;     
        options_.level0_stop_writes_trigger = 99999999;
        options_.max_open_files = -1;
        options_.level0_file_num_compaction_trigger = 4;

        options_.transformer = std::make_shared<rocksdb::Bytecoder>();
        options_.SetTransformType(3);

        uint64_t nums = stoi(props.GetProperty(CoreWorkload::RECORD_COUNT_PROPERTY));
        uint32_t key_len = stoi(props.GetProperty(CoreWorkload::KEY_LENGTH));
        uint32_t value_len = stoi(props.GetProperty(CoreWorkload::FIELD_LENGTH_PROPERTY));
        uint32_t cache_size = nums * (key_len + value_len) / 10;
        if(cache_size < 8 << 20) {
            cache_size = 8 << 20;
        }
        cache_ = rocksdb::NewLRUCache(cache_size);
    }

    void TestFlatBuffers::KeepOnlyRequestedFields(data::Row &row,
                    const std::set<std::string> *fields, data::Row &selectedColumns)
    {
        for (auto field : *fields) {
            for (int i = 0; i < row.columns_size(); i++) {
                if (row.columns(i).name().compare(field) == 0) {
                    data::Column* selectedColumn = selectedColumns.add_columns();
                    selectedColumn->set_name(row.columns(i).name());
                    selectedColumn->set_value(row.columns(i).value());
                    break;
                }
            }
        }
    }

}
