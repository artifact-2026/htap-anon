#include <iostream>
#include <cmath>
#include <queue>
#include <nlohmann/json.hpp>
#include "core/core_workload.h"
#include "test_indexing.h"
#include "lib/coding.h"
#include "transformer/common/parser/json_parser.h"
#include "transformer/common/encoder/json_encoder.h"

using namespace std;

namespace ycsbc {
    Indexing::Indexing(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        int num_cols = utils::StrToInt(props.GetProperty("fieldcount", "1"));
        
        rocksdb::Options options;
        ycsbc::DBHelper::SetOptions(options, true, props);

        options.paranoid_file_checks = false;

        std::vector<std::vector<int>> indexes;
        std::vector<int> index;
        index.push_back(0);
        indexes.push_back(index);
        options.transformers.push_back(std::make_shared<rocksdb::Augmenter>(indexes));

        std::vector<rocksdb::FieldSchema> in_schema; 
        in_schema.reserve(num_cols);
        for (int i = 0; i < num_cols; i++) {
            in_schema.push_back(rocksdb::FieldSchema{"col"+std::to_string(i), "string", i});
        }
        std::vector<std::vector<rocksdb::FieldSchema>> out_schemas;

        auto parser = std::make_shared<rocksdb::JsonColsParser>(num_cols, /*expected_value_len=*/0);
        auto enc = std::make_shared<rocksdb::JsonEncoder>();
        rocksdb::Codec in_codec{parser, nullptr};
        rocksdb::Codec out_codec{nullptr, enc};

        options.schemaDescriptors.push_back(std::make_shared<rocksdb::SchemaDescriptor>(
            in_codec, out_codec, in_schema, out_schemas
        ));
        
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

}
