#include <nlohmann/json.hpp>
#include "core/core_workload.h"
#include "test_mynoop.h"
#include "lib/coding.h"
#include "transformer/identity/mynooper.h"

using namespace std;

namespace ycsbc {
    TestMynoop::TestMynoop(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        int num_cols = utils::StrToInt(props.GetProperty("fieldcount", "1"));
        std::string input_format = props.GetProperty("inputdataformat", "protobuf");
        rocksdb::InputOutputDataType input_type = rocksdb::InputOutputDataType::PROTOBUF;
        if (input_format == "json") {
            input_type = rocksdb::InputOutputDataType::JSON;
        } else if (input_format == "fixedbin64") {
            input_type = rocksdb::InputOutputDataType::FIXEDBIN64;
        }
        
        rocksdb::Options options;
        ycsbc::DBHelper::SetOptions(options, true, props);
        options.transformers.push_back(std::make_shared<rocksdb::Mynooper>());

        std::vector<rocksdb::FieldSchema> in_schema; 
        for (int i = 0; i < num_cols; i++) {
            in_schema.push_back(rocksdb::FieldSchema{"col"+std::to_string(i), "fixedbin64", i});
        }
        options.schemaDescriptors.push_back(std::make_shared<rocksdb::MynooperSchema>(
            input_type, std::move(in_schema)
        ));

        mymBroker_ = std::make_unique<rocksdb::MymBroker>(dbname, !bootstrap, dbfilename, options, 1);
    }

    /*
    * Read is for point query over all columns
    */
    int TestMynoop::Read(const std::string &table, const std::string &key, const std::set<int> *fields,
                      const std::string &req_dist, bool index_access, std::string &result) 
    {
        return mymBroker_->Read(key, fields, result);
    }

    int TestMynoop::Scan(const std::string &table, const std::string &begin_key,
                          const std::string &end_key, const std::set<int> *fields,
                          const std::string &req_dist, bool index_access,
                          std::vector<std::string> &result) 
    {
        return mymBroker_->Scan(begin_key, 100, fields, result);
    }

    int TestMynoop::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        return mymBroker_->Insert(key, values);
    }

    int TestMynoop::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int TestMynoop::Delete(const std::string &table, const std::string &key)
    {
        return mymBroker_->Delete(key);
    }

}
