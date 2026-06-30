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
#include "mycelium/distributor.h"
#include "mycelium/converter.h"
#include "mycelium/json_parser.h"
#include "mycelium/json_encoder.h"
#include "mycelium/protobuf_parser.h"
#include "mycelium/protobuf_encoder.h"

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
        
        options.transformers.push_back(std::make_shared<mycelium::Distributor>(splits));
        options.transformers.push_back(std::make_shared<mycelium::Converter>());

        std::string format = props.GetProperty("inputdataformat", "protobuf");
        std::shared_ptr<mycelium::Parser> parser, parser2;
        std::shared_ptr<mycelium::Encoder> enc, enc2;
        if (format == "json") {
            parser = std::make_shared<mycelium::JsonColsParser>(fieldcount, /*expected_value_len=*/0);
            enc = std::make_shared<mycelium::JsonEncoder>();
            parser2 = std::make_shared<mycelium::JsonColsParser>(fieldcount/2, /*expected_value_len=*/0);
            enc2 = std::make_shared<mycelium::ProtobufBytesRowEncoder>(fieldcount/2);
        } else {
            parser = std::make_shared<mycelium::ProtobufParser>(std::make_unique<data::ByteRow>());
            enc = std::make_shared<mycelium::ProtobufBytesRowEncoder>(fieldcount);
            parser2 = std::make_shared<mycelium::ProtobufParser>(std::make_unique<data::BytesRow>());
            enc2 = std::make_shared<mycelium::ProtobufBytesRowEncoder>(fieldcount/2);
        }

        mycelium::Codec in_codec{parser, nullptr}, out_codec{nullptr, enc};

        std::vector<mycelium::FieldSchema> in_schema; 
        for (int i = 0; i < fieldcount; i++) {
            in_schema.push_back(mycelium::FieldSchema{"col"+std::to_string(i), "string", i});
        }
        std::vector<std::vector<mycelium::FieldSchema>> out_schemas;
        
        options.schemaDescriptors.push_back(std::make_shared<mycelium::SchemaDescriptor>(
            in_codec, out_codec, in_schema, out_schemas));

        mycelium::Codec in2_codec{parser2, nullptr};
        mycelium::Codec out2_codec{nullptr, enc2};

        std::vector<mycelium::FieldSchema> input_schema = parser->GetInputFieldSchema();
        std::vector<std::vector<mycelium::FieldSchema>> output_schemas;  // empty
        options.schemaDescriptors.push_back(
                std::make_shared<mycelium::SchemaDescriptor>(in2_codec, out2_codec,
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
