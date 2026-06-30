#include "core/core_workload.h"
#include "test_rocks_db.h"
#include "lib/coding.h"
#include "test_converting.h"
#include "mycelium/converter.h"
#include "mycelium/json_parser.h"
#include "mycelium/protobuf_parser.h"
#include "mycelium/protobuf_encoder.h"

#include <iostream>
#include <iomanip> // Include for std::setfill and std::setw

using namespace std;

namespace ycsbc {
    TestFlatBuffers::TestFlatBuffers(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        int num_cols = utils::StrToInt(props.GetProperty("fieldcount", "10"));
        
        rocksdb::Options options;
        ycsbc::DBHelper::SetOptions(options, true, props);
        
        options.transformers.push_back(std::make_shared<mycelium::Converter>());

        //std::unique_ptr<google::protobuf::Message> input_proto_template = std::make_unique<data::ByteRow>();
        //const flatbuffers::TypeTable* fb_type_table = flat::RowTypeTable();

        std::string format = props.GetProperty("inputdataformat", "protobuf");
        std::shared_ptr<mycelium::Parser> parser;
        if (format == "json") {
            parser = std::make_shared<mycelium::JsonColsParser>(num_cols, /*expected_value_len=*/0);
        } else {
            parser = std::make_shared<mycelium::ProtobufParser>(std::make_unique<data::ByteRow>());
        }
        auto enc = std::make_shared<mycelium::ProtobufBytesRowEncoder>(num_cols);
        mycelium::Codec in{parser, nullptr};
        mycelium::Codec out{nullptr, enc};

        std::vector<mycelium::FieldSchema> in_schema = parser->GetInputFieldSchema();
        std::vector<std::vector<mycelium::FieldSchema>> out_schemas;  // empty
        options.schemaDescriptors.push_back(
                std::make_shared<mycelium::SchemaDescriptor>(in, out,
                        std::move(in_schema), std::move(out_schemas)));

        mymBroker_ = std::make_unique<rocksdb::MymBroker>(dbname, !bootstrap, dbfilename, options, 1); 
    }

    /*
    * Read is for point query over all columns
    */
    int TestFlatBuffers::Read(const std::string &table, const std::string &key, const std::set<int> *fields,
                      const std::string &req_dist, bool index_access, std::string &result) 
    {
        return mymBroker_->Read(key, fields, result);
    }

    int TestFlatBuffers::Scan(const std::string &table, const std::string &begin_key,
                          const std::string &end_key, const std::set<int> *fields,
                          const std::string &req_dist, bool index_access,
                          std::vector<std::string> &result) 
    {
        return mymBroker_->Scan(begin_key, 100, fields, result);
    }

    int TestFlatBuffers::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        return mymBroker_->Insert(key, values);
    }

    int TestFlatBuffers::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int TestFlatBuffers::Delete(const std::string &table, const std::string &key)
    {
        return mymBroker_->Delete(key);
    }

}
