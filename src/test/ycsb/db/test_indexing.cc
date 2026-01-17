#include <iostream>
#include <cmath>
#include <queue>
#include <nlohmann/json.hpp>
#include "core/core_workload.h"
#include "test_indexing.h"
#include "lib/coding.h"

using namespace std;

namespace ycsbc {
    Indexing::Indexing(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        std::string data_format = props.GetProperty("inputdataformat","protobuf");
        
        rocksdb::Options options;
        ycsbc::DBHelper::SetOptions(options, true, props);

        options.paranoid_file_checks = false;

        std::vector<std::unique_ptr<rocksdb::DeriveFuncData>> deriveFuncs;
        deriveFuncs.push_back(CreateIndexer(std::vector<int>(3)));
        options.transformers.push_back(std::make_shared<rocksdb::Augmenter>());

        if (data_format == "json") {
            std::vector<std::string> index_keys;
            index_keys.push_back("col1");
            std::vector<std::vector<std::string>> indexes;
            indexes.push_back(index_keys);
            options.schemaDescriptors.push_back(std::make_shared<rocksdb::JsonAugmenterSchema>(indexes,nlohmann::json::object()));
        } else if (data_format == "protobuf") {
            std::vector<int> index_keys;
            index_keys.push_back(1);
            std::vector<std::vector<int>> indexes;
            indexes.push_back(index_keys);
            options.schemaDescriptors.push_back(std::make_shared<rocksdb::ProtobufAugmenterSchema>(
                                    indexes, std::make_unique<data::ByteRow>()));
        }
        
        mymBroker_ = std::make_unique<rocksdb::MymBroker>(dbname, !bootstrap, dbfilename, options, 1); 
    }

    /*
    * Read is for point query over all columns
    * Here Read will find the first key/value pair that the index is pointing to and return.
    */
    int Indexing::Read(const std::string &table, const std::string &key,
                        const std::set<int> *fields, const std::string &req_dist,
                        bool index_access, std::string &result) 
    {
        return mymBroker_->Read(key, fields, result);
    }

    int Indexing::Scan(const std::string &table, const std::string &begin_key,
                       const std::string &end_key, const std::set<int> *fields,
                       const std::string &req_dist, bool index_access,
                       std::vector<std::string> &result) 
    {
      return mymBroker_->Scan(begin_key, 100, fields, result);
    }

    int Indexing::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        return mymBroker_->Insert(key, values);
    }

    int Indexing::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int Indexing::Delete(const std::string &table, const std::string &key)
    {
        return mymBroker_->Delete(key);
    }

    std::vector<std::string> Indexing::deserializeIndex(const std::string& serialized)
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

    std::vector<std::string> Indexing::parsePrimaryKeys(const std::string& value) {
        std::vector<std::string> primary_keys;
        std::istringstream stream(value);
        std::string key;
    
        while (std::getline(stream, key, ',')) {
            primary_keys.push_back(key);
        }

        return primary_keys;
    }

    std::unique_ptr<rocksdb::DeriveFuncData> Indexing::CreateIndexer(std::vector<int> positions) {
      auto f = [positions = std::move(positions)](std::vector<std::string>& strs) -> std::string {
        if (strs.empty()) return "";
        if (strs.size() == 1) return strs[0];

        std::string ind = strs[0];
        size_t total_length = ind.size();
        for (size_t i = 1; i < strs.size(); ++i) total_length += 1 + strs[i].size();
        ind.reserve(total_length);

        for (size_t i = 1; i < strs.size(); ++i) {
            ind += ",";
            ind += strs[i];
        }
        return ind;
      };

      return std::make_unique<rocksdb::DeriveFuncData>(std::move(positions), std::move(f));
    }

}
