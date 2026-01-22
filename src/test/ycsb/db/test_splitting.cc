#include "core/core_workload.h"
#include "test_splitting.h"
#include "transformer/distribute/distributor.h"
#include "transformer/common/parser/json_parser.h"
#include "transformer/common/encoder/json_encoder.h"

using namespace std;

namespace ycsbc {
    TestSplitting::TestSplitting(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        int fieldcount = utils::StrToInt(props.GetProperty("fieldcount", "2"));

        rocksdb::Options options;
        ycsbc::DBHelper::SetOptions(options, true, props);

        std::vector<std::vector<int>> splits;
        std::vector<std::vector<rocksdb::FieldSchema>> out_schemas;
        if (fieldcount <= 1) {
            splits.reserve(1);
            std::vector<int> split1;
            split1.reserve(1);
            split1.push_back(0);
            splits.push_back(split1);

            out_schemas.reserve(1);
            std::vector<rocksdb::FieldSchema> out_schema1;
            out_schema1.reserve(1);
            out_schema1.push_back(rocksdb::FieldSchema{"col0", "string", 0});
            out_schemas.push_back(out_schema1);
        } else {
            splits.reserve(2);
            std::vector<int> split1, split2;
            out_schemas.reserve(2);
            std::vector<rocksdb::FieldSchema> out_schema1, out_schema2;

            const int mid = fieldcount/2;
            split2.reserve(mid);
            split1.reserve(fieldcount-mid);
            out_schema2.reserve(mid);
            out_schema1.reserve(fieldcount-mid);

            for (int i = 0; i < fieldcount; i++) {
                split1.push_back(i);
                out_schema1.push_back(rocksdb::FieldSchema{"col"+std::to_string(i), "string", i});
                i += 1;
                if (i < fieldcount) {
                    split2.push_back(i);
                    out_schema2.push_back(rocksdb::FieldSchema{"col"+std::to_string(i), "string", i});
                }    
            }
            splits.push_back(split1);
            splits.push_back(split2);
            out_schemas.push_back(out_schema1);
            out_schemas.push_back(out_schema2);
        }

        options.transformers.push_back(std::make_shared<rocksdb::Distributor>(splits));
        
        auto parser = std::make_shared<rocksdb::JsonColsParser>(fieldcount, /*expected_value_len=*/0);
        auto enc = std::make_shared<rocksdb::JsonEncoder>();
        rocksdb::Codec codec_in{parser, nullptr};
        rocksdb::Codec codec_out{nullptr, enc};

        std::vector<rocksdb::FieldSchema> in_schema; 
        in_schema.reserve(fieldcount);
        for (int i = 0; i < fieldcount; i++) {
            in_schema.push_back(rocksdb::FieldSchema{"col"+std::to_string(i), "string", i});
        }

        options.schemaDescriptors.push_back(std::make_shared<rocksdb::SchemaDescriptor>(
            codec_in, codec_out, in_schema, out_schemas));

        mymBroker_ = std::make_unique<rocksdb::MymBroker>(dbname, !bootstrap, dbfilename, options, 2);
    }

    /*
    * Read is for point query over all columns
    */
    int TestSplitting::Read(const std::string &table, const std::string &key, const std::set<int> *fields,
                      const std::string &req_dist, bool index_access, std::string &result)
    {
        return mymBroker_->Read(key, fields, result);
    }

    int TestSplitting::Scan(const std::string &table, const std::string &begin_key,
                          const std::string &end_key, const std::set<int> *fields,
                          const std::string &req_dist, bool index_access,
                          std::vector<std::string> &result) 
    {
        return mymBroker_->Scan(begin_key, 100, fields, result);
    }

    int TestSplitting::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        return mymBroker_->Insert(key, values);
    }

    int TestSplitting::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int TestSplitting::Delete(const std::string &table, const std::string &key)
    {
        return mymBroker_->Delete(key);
    }
}
