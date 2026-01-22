#include <cmath>
#include <queue>
#include <future>

#include "core/core_workload.h"
#include "test_fb_cracker.h"
#include "lib/coding.h"
#include <nlohmann/json.hpp>

#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "transformer/distribute/distributor.h"
#include "transformer/convert/converter.h"
#include "transformer/common/parser/json_parser.h"
#include "transformer/common/encoder/json_encoder.h"
#include "transformer/common/encoder/protobuf_encoder.h"

using namespace std;

namespace ycsbc {
    TestFbCracker::TestFbCracker(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        int fieldcount = utils::StrToInt(props.GetProperty("fieldcount", "2"));
        
        rocksdb::Options options;
        ycsbc::DBHelper::SetOptions(options, true, props);

        std::vector<std::vector<int>> splits;
        int mid = fieldcount/2;
        std::vector<int> split1, split2;
        for (int j = 0; j < mid; j++) {
            split1.push_back(j);
            split2.push_back(j+mid);
        }
        if (mid * 2 < fieldcount) {
            split2.push_back(fieldcount-1);
        }
        splits.push_back(split1);
        splits.push_back(split2);
        
        options.transformers.push_back(std::make_shared<rocksdb::Distributor>(splits));
        options.transformers.push_back(std::make_shared<rocksdb::Converter>());

        auto parser = std::make_shared<rocksdb::JsonColsParser>(fieldcount, /*expected_value_len=*/0);
        auto enc = std::make_shared<rocksdb::JsonEncoder>();
        rocksdb::Codec in_codec{parser, nullptr}, out_codec{nullptr, enc};

        std::vector<rocksdb::FieldSchema> in_schema; 
        for (int i = 0; i < fieldcount; i++) {
            in_schema.push_back(rocksdb::FieldSchema{"col"+std::to_string(i), "string", i});
        }
        std::vector<std::vector<rocksdb::FieldSchema>> out_schemas;
        
        options.schemaDescriptors.push_back(std::make_shared<rocksdb::SchemaDescriptor>(
            in_codec, out_codec, in_schema, out_schemas));

        auto parser2 = std::make_shared<rocksdb::JsonColsParser>(fieldcount/2, /*expected_value_len=*/0);
        auto enc2 = std::make_shared<rocksdb::ProtobufBytesRowEncoder>(fieldcount/2);
        rocksdb::Codec in2_codec{parser2, nullptr};
        rocksdb::Codec out2_codec{nullptr, enc2};

        std::vector<rocksdb::FieldSchema> input_schema = parser->GetInputFieldSchema();
        std::vector<std::vector<rocksdb::FieldSchema>> output_schemas;  // empty
        options.schemaDescriptors.push_back(
                std::make_shared<rocksdb::SchemaDescriptor>(in2_codec, out2_codec,
                        std::move(input_schema), std::move(output_schemas)));

        mymBroker_ = std::make_unique<rocksdb::MymBroker>(
                dbname, !bootstrap, dbfilename, options, 2);
    }

    /*
    * Read is for point query over all columns
    */
    int TestFbCracker::Read(const std::string &table, const std::string &key, const std::set<int> *fields,
                      const std::string &req_dist, bool index_access, std::string &result)
    {
        return mymBroker_->Read(key, fields, result);
    }

    int TestFbCracker::Scan(const std::string &table, const std::string &begin_key,
                          const std::string &end_key, const std::set<int> *fields,
                          const std::string &req_dist, bool index_access,
                          std::vector<std::string> &result) 
    {
        return mymBroker_->Scan(begin_key, 100, fields, result);
    }

    int TestFbCracker::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        return mymBroker_->Insert(key, values);
    }

    int TestFbCracker::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int TestFbCracker::Delete(const std::string &table, const std::string &key)
    {
        return mymBroker_->Delete(key);
    }

}
