#include <nlohmann/json.hpp>
#include "core/core_workload.h"
#include "test_preindexing.h"
#include "lib/coding.h"

using namespace std;

namespace ycsbc {
    TestPreindexing::TestPreindexing(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        noResults = 0;
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        int levels = utils::StrToInt(props.GetProperty("levels", "6"));
        int fieldcount = utils::StrToInt(props.GetProperty("fieldcount", "16"));
        inputType_ = props.GetProperty("inputdataformat", "protobuf");
        outputType_ = props.GetProperty("outputdataformat", "protobuf");
        columnDataType_ = props.GetProperty("columndatatype", "numeric");
        SetOptions(dbfilename, levels, fieldcount, false);

        std::vector<rocksdb::ColumnFamilyDescriptor> column_family_descriptors;
        GetColumnFamilyDescriptors(dbname, column_family_descriptors);
        std::vector<rocksdb::ColumnFamilyHandle*> cf_handles;

        if (bootstrap) {
            rocksdb::Status s = rocksdb::DB::Open(options_, 
                                              dbfilename,
                                              &rocksdb_);
            if (!s.ok()){
                std::cerr<<"Can't open preindexing "<<dbfilename<<" "<<s.ToString()<<std::endl;
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
                std::cerr<<"Can't open preindexing "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }
        }
        BuildColumnFamilyHandleMap(column_family_descriptors, cf_handles);
    }

    /*
    * Read is for point query over all columns
    */
    int TestPreindexing::Read(const std::string &table, const std::string &key, const std::set<int> *fields,
                      const std::string &req_dist, bool index_access, std::string &result) 
    {
        rocksdb::Status s, t;
        if (index_access) {
            std::string valuekeysstr;
            s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table+"_index_cf"], key, &valuekeysstr);

            std::vector<std::string> valuekeys = parsePrimaryKeys(valuekeysstr);
            for (auto valuekey : valuekeys) {
                t = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table], valuekey, &result);
            }
            if (s.ok()) {
                return 0;
            }
        } else {
            s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table], key, &result);
        }

        return 1;
    }

    int TestPreindexing::Scan(const std::string &table, const std::string &begin_key,
                          const std::string &end_key, const std::set<int> *fields,
                          const std::string &req_dist, bool index_access,
                          std::vector<std::string> &result) 
    {
        result.clear();
        rocksdb::Status s;

        if (index_access) {
            int searched = 0;
            std::set<std::string> foundkeys;
            auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandles_[table+"_index_cf"]);
            it->Seek(begin_key);
            while (it->Valid() && searched < 25) {
                std::vector<std::string> rowkeys = parsePrimaryKeys(it->value().ToString());
                for (auto k : rowkeys) {
                    foundkeys.insert(k);
                }
                
                it->Next();
                searched++;
            }

            for (auto fk : foundkeys) {
                std::string fvalue;
                s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table], fk, &fvalue);
                result.push_back(fvalue);
            }
            if (result.size() > 0) {
                return 0;
            }
        } else {
            auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandles_[table]);
            int searched = 0;
            it->Seek(begin_key);
            while (it->Valid() && searched < 100) {
                uint64_t sum = 0;
                if (fields != nullptr) {
                    if (inputType_ == "protobuf") {
                        data::Row row;
                        row.ParseFromString(it->value().ToString());
                        sum += std::stoi(row.columns(0));
                    } else {
                        nlohmann::json parsedJson = nlohmann::json::parse(it->value().ToString());
                        sum += std::stoi(parsedJson["field0"].get<std::string>());
                    }
                }
                result.push_back(it->value().ToString());

                it->Next();
                searched++;
            }
            if (result.size() > 100) {
                return 0;
            }
        }
        return 1;
    }

    int TestPreindexing::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        std::string ikey = "";
        if (inputType_ == "protobuf") {
            data::Row row;
            row.ParseFromString(values);
            if (row.columns_size() > 0) {
                ikey = row.columns(0);
            } else {
                return 1;
            }
        } else {
            nlohmann::json parsedJson = nlohmann::json::parse(values);
            ikey = parsedJson["field0"].get<std::string>();
        }

        rocksdb::Status s = rocksdb_->Merge(write_options_, cfhandles_[table+"_index_cf"], ikey, key);
        if (!s.ok()) {
            return 1;
        }

        s = rocksdb_->Put(write_options_, cfhandles_[table], key, values);
        if (!s.ok()) {
            return 1;
        }

        return 0;

        // find out if this key was indexed before
        /*std::string indexed;
        s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table+"_derived_cf_0-helper"], key, &indexed);
        if (indexed != "" && indexed != ikey) {
            // remove the old indexed value
            std::string removekeysstr;
            s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table+"_derived_cf_0"], indexed, &removekeysstr);

            std::vector<std::string> removekeys = deserializeIndex(removekeysstr);
            removekeys.erase(std::remove(removekeys.begin(), removekeys.end(), key), removekeys.end());

            // write back the remaining keys after the removal
            if (removekeys.size() > 0) {
                std::ostringstream oss;

                size_t sz = removekeys.size();
                oss.write(reinterpret_cast<const char*>(&sz), sizeof(sz));

                for (const auto& k : removekeys) {
                    size_t klen = k.size();
                    oss.write(reinterpret_cast<const char*>(&klen), sizeof(klen));
                    oss.write(k.c_str(), klen);
                }
                s = rocksdb_->Put(rocksdb::WriteOptions(), cfhandles_[table+"_derived_cf_0"], indexed, oss.str());
            }
        } else if (indexed == "") {
            // key was never indexed before so we add it
            std::string indvalues;
            s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table+"_index_cf"], ikey, &indvalues);

            std::ostringstream oss;
            size_t key_len = key.size();
            oss.write(reinterpret_cast<const char*>(&key_len), sizeof(key_len));  // Write string length
            oss.write(key.c_str(), key_len);

            indvalues += oss.str();

            s = rocksdb_->Put(rocksdb::WriteOptions(), cfhandles_[table+"_index_cf"], ikey, indvalues);
            if (s.ok()) {
                s = rocksdb_->Put(rocksdb::WriteOptions(), cfhandles_[table], key, values);
                if (s.ok()) {
                    return 0;
                }
            }
        }

        // index was taken care of, now insert the data
        s = rocksdb_->Put(rocksdb::WriteOptions(), cfhandles_[table], key, values);
        if (s.ok()) {
            return 0;
        }

        return 1;*/
    }

    int TestPreindexing::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int TestPreindexing::Delete(const std::string &table, const std::string &key)
    {
        rocksdb::Status s;
        // Get the value pointed by the primary key
        std::string value;
        s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table], key, &value);
        if (!s.ok()) {
            return 1;
        }

        // parse the value to get the key in the secondary index
        data::Row row;
        row.ParseFromString(value);
        const std::string ikey = row.columns(0);

        // remove primary key from the secondary index
        std::string pkeys;
        s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table+"_index_cf"], ikey, &pkeys);
        if (!s.ok()) {
            return 1;
        }
        removePrimaryKeyFromList(pkeys, key);

        // write back the remaining pkey list to the secondary index
        s = rocksdb_->Put(write_options_, cfhandles_[table+"_index_cf"], ikey, pkeys);
        if (!s.ok()) {
            return 1;
        }

        // finally delete the key from primary data
        s = rocksdb_->Delete(write_options_, cfhandles_[table], key);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    void TestPreindexing::SetOptions(const char *dbfilename, int levels, int fieldcount, bool logging)
    {
        if (!logging) {
            options_.info_log_level = rocksdb::InfoLogLevel::FATAL_LEVEL;
        }

        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;
        options_.max_open_files = -1;
        options_.env->SetBackgroundThreads(16, rocksdb::Env::Priority::LOW);
	    options_.env->SetBackgroundThreads(8, rocksdb::Env::Priority::HIGH);
	    options_.max_background_compactions = 16;
	    options_.max_background_flushes = 8;

	    options_.max_subcompactions = 16;

        options_.num_levels = levels;
        options_.num_columns = fieldcount;
        options_.SetTransformerType(rocksdb::TransformerType::NOTRANSFORMATION);
        options_.SetInputOutputDataType(ycsbc::DBHelper::mapStringToDataType(inputType_),
                                        ycsbc::DBHelper::mapStringToDataType(outputType_));

        options_.write_buffer_size = 128 * 1024 * 1024;
        options_.max_write_buffer_number = 8;
        options_.level0_file_num_compaction_trigger = 4;
        options_.level0_slowdown_writes_trigger = 20;
        options_.level0_stop_writes_trigger = 32;
        options_.IncreaseParallelism(24);
        options_.use_direct_reads = true;
        options_.use_direct_io_for_flush_and_compaction = true;

        options_.target_file_size_base = 256 * 1024 * 1024;
        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_cache = rocksdb::NewLRUCache(512 * 1024 * 1024);
        table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
        options_.table_factory = std::shared_ptr<rocksdb::TableFactory>(rocksdb::NewBlockBasedTableFactory(table_options));
    }

    void TestPreindexing::GetColumnFamilyDescriptors(const std::string& dbname,
                    std::vector<rocksdb::ColumnFamilyDescriptor>& column_families)
    {
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(dbname,
                                                                  rocksdb::ColumnFamilyOptions(options_)));
        options_.merge_operator = std::make_shared<SecondaryPreindexMergeOperator>();
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(dbname+"_index_cf",
                                                                  rocksdb::ColumnFamilyOptions(options_)));
        //column_families.push_back(rocksdb::ColumnFamilyDescriptor(dbname+"_derived_cf_0-helper",
        //                                                          rocksdb::ColumnFamilyOptions(options_)));                   
    }

    void TestPreindexing::BuildColumnFamilyHandleMap(std::vector<rocksdb::ColumnFamilyDescriptor>& column_family_descriptors,
                            std::vector<rocksdb::ColumnFamilyHandle*> handles)
    {
        for (size_t i = 0; i < handles.size(); i++) {
            if (column_family_descriptors[i].name != rocksdb::kDefaultColumnFamilyName) {
                cfhandles_.insert({column_family_descriptors[i].name, handles[i]});
            }
        }
    }

    std::vector<std::string> TestPreindexing::parsePrimaryKeys(const std::string& value) {
        std::vector<std::string> primary_keys;
        std::istringstream stream(value);
        std::string key;
    
        while (std::getline(stream, key, ',')) {
            primary_keys.push_back(key);
        }

        return primary_keys;
    }

    void TestPreindexing::removePrimaryKeyFromList(std::string& value, const std::string& pkey) {
        size_t start_index = value.find(pkey);
        if (start_index == std::string::npos) {
            return;  // pkey not found, so no modification needed
        }

        // Find the comma immediately after pkey
        size_t end_index = value.find_first_of(',', start_index + pkey.length());

        if (end_index == std::string::npos) {
            // pkey is the last item in the list, remove it including the preceding comma if any
            if (start_index > 0 && value[start_index - 1] == ',') {
                start_index -= 1;  // Include the comma before pkey
            }
            value = value.substr(0, start_index);
        } else {
            // pkey is followed by other items, remove it including the following comma
            value = value.substr(0, start_index) + value.substr(end_index + 1);
        }
    }

    std::vector<std::string> TestPreindexing::deserializeIndex(const std::string& serialized)
    {
        std::istringstream iss(serialized);
        std::vector<std::string> result;

        size_t num_strings;
        iss.read(reinterpret_cast<char*>(&num_strings), sizeof(num_strings));

        for (size_t i = 0; i < num_strings; i++) {
            size_t str_len;
            iss.read(reinterpret_cast<char*>(&str_len), sizeof(str_len));

            std::string str(str_len, '\0');
            iss.read(&str[0], str_len);

            result.push_back(str);
        }

        return result;
    }

    std::set<int> TestPreindexing::GetQueryingHandles(std::set<std::string> fields) {
        std::set<int> fieldpositions;
        for (auto field : fields) {
            int pos = 0;
            for (size_t i = 5; i < field.size(); i++) {
                pos = pos*10 + field[i] - '0';
            }
            fieldpositions.insert(pos/2);
        }
        return fieldpositions;
    }

}
