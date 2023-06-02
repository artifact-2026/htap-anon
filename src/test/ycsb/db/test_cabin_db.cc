#include "core/core_workload.h"
#include "test_cabin_db.h"
#include "lib/coding.h"

using namespace std;

namespace ycsbc {

    TestCabinDB::TestCabinDB(const char *dbfilename, utils::Properties &props) {
        rocksdb::Options options;
        SetOptions(&options, props);

        int num_cfshards = stoi(props.GetProperty("fieldcount", "0"));
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","true"));

        cabindb_ = new cabindb::CabinDB(dbfilename, options, num_cfshards, bootstrap);
    }

    void TestCabinDB::SetOptions(rocksdb::Options *options, utils::Properties &props) {

        options->create_if_missing = true;
        options->compression = rocksdb::kNoCompression;
        //options->enable_pipelined_write = true;

        //options->max_background_jobs = 2;
        //options->max_bytes_for_level_base = 32ul * 1024 * 1024;
        //options->write_buffer_size = 32ul * 1024 * 1024;
        //options->max_write_buffer_number = 2;
        //options->target_file_size_base = 4ul * 1024 * 1024;

	    options->num_levels = stoi(props.GetProperty("levels", "4"));
        //options->level0_file_num_compaction_trigger = 4;
        //options->level0_slowdown_writes_trigger = 8;     
        //options->level0_stop_writes_trigger = 12;

        //options->use_direct_reads = true;
        //options->use_direct_io_for_flush_and_compaction = true;

        uint64_t nums = stoi(props.GetProperty(CoreWorkload::RECORD_COUNT_PROPERTY));
        uint32_t key_len = stoi(props.GetProperty(CoreWorkload::KEY_LENGTH));
        uint32_t value_len = stoi(props.GetProperty(CoreWorkload::FIELD_LENGTH_PROPERTY));
        uint32_t cache_size = nums * (key_len + value_len) * 10 / 100;
        if(cache_size < 8 << 20) {
            cache_size = 8 << 20;
        }
        cache_ = rocksdb::NewLRUCache(cache_size);

        bool statistics = utils::StrToBool(props["dbstatistics"]);
        if(statistics){
            dbstats_ = rocksdb::CreateDBStatistics();
            options->statistics = dbstats_;
        }
    }

    int TestCabinDB::Read(const std::string &table, const std::string &key, const std::vector<std::string> *fields,
                      data::Row &result)
    {
        cabindb::Status s;
        std::string value = "";
        std::vector<std::vector<std::string> > columnfamilies = cabindb_->GetColumnFamilyNames();

        if (fields == NULL) { // read all columns
            for (uint64_t i = 0; i < columnfamilies.size(); i++) {
                std::string foundValue;
                for (auto cfname : columnfamilies[i]) {
                    foundValue.clear();
                    s = cabindb_->Read(cfname, key, foundValue);
                    if (s == cabindb::Status::kOK) {
                        value += foundValue;
                    } else if (s == cabindb::Status::kNotFound) {
                        break;
                    } else {
                        return -1;
                    }
                }
                if (s == cabindb::Status::kOK && value != "") {
                    return 0;
                }
            }
        } else { // read subset columns
            for (uint64_t i = 0; i < columnfamilies.size(); i++) {
                std::set<int> cf_positions_on_the_level;
                if (i == 0) {
                    cf_positions_on_the_level.insert(0);
                } else {
                    GetColumnFamiliesOnOneLevel(fields, i, cf_positions_on_the_level);
                }

                for (uint64_t j = 0; j < columnfamilies[i].size(); j++) {
                    if (cf_positions_on_the_level.find(j) != cf_positions_on_the_level.end()) {
                        continue;
                    }

                    std::string foundValue;
                    s = cabindb_->Read(columnfamilies[i][j], key, foundValue);
                    if (s == cabindb::Status::kOK) {
                        value += foundValue;
                    } else if (s == cabindb::Status::kNotFound) {
                        break;
                    } else {
                        return -1;
                    }
                }
                if (s == cabindb::Status::kOK && value != "") {
                    return 0;
                }
            }
        }

        noResults++;
        return 0;
    }

    int TestCabinDB::Scan(const std::string &table, const std::string &key, int len,
                      const std::vector<std::string> *fields,
                      std::vector<data::Row> &result) 
    {
        std::vector<std::string> values;
        for (int i=0; i < len; i++) {
            values.push_back("");
        }
        cabindb::Status s;
        std::vector<std::vector<std::string> > columnfamilies = cabindb_->GetColumnFamilyNames();

        if (fields == NULL) { // scan all columns
            for (uint64_t i = 0; i < columnfamilies.size(); i++) {
                std::vector<std::string> foundValues;
                for (auto cfname : columnfamilies[i]) {
                    foundValues.clear();
                    s = cabindb_->Scan(cfname, key, len, foundValues);
                    if (s == cabindb::Status::kOK) {
                        for (int j=0; j<len; j++) {
                            values[j] += foundValues[j];
                        }
                    } else if (s == cabindb::Status::kNotFound) {
                        break;
                    } else {
                        return -1;
                    }
                }
                if (s == cabindb::Status::kOK && values.size() != 0) {
                    return 0;
                }
            }
        } else { // scan subset of columns
            for (uint64_t i = 0; i < columnfamilies.size(); i++) {
                std::set<int> cf_positions_on_the_level;
                if (i == 0) {
                    cf_positions_on_the_level.insert(0);
                } else {
                    GetColumnFamiliesOnOneLevel(fields, i, cf_positions_on_the_level);
                }

                for (uint64_t j = 0; j < columnfamilies[i].size(); j++) {
                    if (cf_positions_on_the_level.find(j) != cf_positions_on_the_level.end()) {
                        continue;
                    }

                    std::vector<std::string> foundValues;
                    s = cabindb_->Scan(columnfamilies[i][j], key, len, foundValues);
                    if (s == cabindb::Status::kOK) {
                        for (int k=0; k<len; k++) {
                            values[k] += foundValues[k];
                        }
                    } else if (s == cabindb::Status::kNotFound) {
                        break;
                    } else {
                        return -1;
                    }
                }
                if (s == cabindb::Status::kOK && values.size() != 0) {
                    return 0;
                }
            }
        }

        noResults++;
        return 0;
    }

    int TestCabinDB::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        std::string value;
        //SerializeValue(values, value);

        cabindb::Status s = cabindb_->Insert(table, key, value);
        if (s == cabindb::Status::kOK) {
            return 0;
        }

        return 1;
    }

    int TestCabinDB::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int TestCabinDB::Delete(const std::string &table, const std::string &key)
    {
        cabindb::Status s = cabindb_->Delete(table, key);
        if (s == cabindb::Status::kOK) {
            return 0;
        }
        return 1;
    }

    void TestCabinDB::StitchColumns(std::vector<std::string> &values, std::vector<KVPair> &kvs)
    {
        kvs.clear();
        uint64_t offset = 0;
        for (unsigned int i = 0; i < values.size(); i++) {
            uint64_t kv_num = DecodeFixed64(values[i].c_str());
            offset += 8;

            for (unsigned int j = 0; j < kv_num; j++) {
                ycsbc::DB::KVPair pair;

                uint64_t key_size = DecodeFixed64(values[i].c_str() + offset);
                offset += 8;

                pair.first.assign(values[i].c_str() + offset, key_size);
                offset += key_size;

                uint64_t value_size = DecodeFixed64(values[i].c_str() + offset);
                offset += 8;

                pair.second.assign(values[i].c_str() + offset, value_size);

                kvs.push_back(pair);
            }
        }
    }

    void TestCabinDB::PopulateFieldToColumnFamilyNameMap(utils::Properties &props)
    {
        int field_count = stoi(props.GetProperty("fieldcount", "0"));
        int num_levels = stoi(props.GetProperty("levels", "0"));
        for (int i = 0; i < field_count; i++) {
            std::map<int, int> columnFamilyPositionsOnLevel;
            for (int j = 0; j < num_levels; j++) {
                columnFamilyPositionsOnLevel[j] = i / (field_count/pow(2, j));
            }
            field_to_cfpositions_map_["field"+std::to_string(i)] = columnFamilyPositionsOnLevel;
        }
    }

    void TestCabinDB::GetColumnFamiliesOnOneLevel(const std::vector<std::string> *fields,
                                         int level, std::set<int> &cf_positions_on_the_level)
    {
        cf_positions_on_the_level.clear();
        for (auto field : *fields) {
            cf_positions_on_the_level.insert(field_to_cfpositions_map_[field][level]);
        }
    }

}
