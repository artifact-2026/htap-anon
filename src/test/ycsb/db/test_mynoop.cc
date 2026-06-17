#include <nlohmann/json.hpp>
#include "core/core_workload.h"
#include "test_mynoop.h"
#include "lib/coding.h"
#include "mycelium/mynooper.h"
#include "mycelium/json_parser.h"
#include "mycelium/json_encoder.h"
#include "mycelium/protobuf_parser.h"
#include "mycelium/protobuf_encoder.h"

using namespace std;

namespace ycsbc {
    TestMynoop::TestMynoop(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        int num_cols = utils::StrToInt(props.GetProperty("fieldcount", "1"));
        
        rocksdb::Options options;
        ycsbc::DBHelper::SetOptions(options, true, props);
        options.transformers.push_back(std::make_shared<mycelium::Mynooper>());

        std::vector<mycelium::FieldSchema> in_schema; 
        for (int i = 0; i < num_cols; i++) {
            in_schema.push_back(mycelium::FieldSchema{"col"+std::to_string(i), "string", i});
        }
        std::vector<std::vector<mycelium::FieldSchema>> out_schemas;
        out_schemas.push_back(in_schema);

        std::string format = props.GetProperty("inputdataformat", "protobuf");
        std::shared_ptr<mycelium::Parser> parser;
        std::shared_ptr<mycelium::Encoder> enc;
        if (format == "json") {
            parser = std::make_shared<mycelium::JsonColsParser>(num_cols, /*expected_value_len=*/0);
            enc = std::make_shared<mycelium::JsonEncoder>();
        } else {
            parser = std::make_shared<mycelium::ProtobufParser>(std::make_unique<data::ByteRow>());
            enc = std::make_shared<mycelium::ProtobufBytesRowEncoder>(num_cols);
        }

        mycelium::Codec in_codec{parser, nullptr};
        mycelium::Codec out_codec{nullptr, enc};
        options.schemaDescriptors.push_back(std::make_shared<mycelium::SchemaDescriptor>(
            in_codec, out_codec, std::move(in_schema), std::move(out_schemas)
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
