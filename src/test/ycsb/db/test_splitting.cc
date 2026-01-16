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

        options.transformers.push_back(std::make_shared<rocksdb::Distributor>());
        
        auto parser = std::make_shared<rocksdb::JsonColsParser>(fieldcount, /*expected_value_len=*/0);
        auto enc = std::make_shared<rocksdb::JsonEncoder>();
        rocksdb::Codec codec{parser, enc};

        std::vector<rocksdb::FieldSchema> in_schema; 
        for (int i = 0; i < fieldcount; i++) {
            in_schema.push_back(rocksdb::FieldSchema{"col"+std::to_string(i), "string", i});
        }
        std::vector<std::vector<int>> splits;
        splits.reserve(2);
        const int mid = fieldcount/2;

        std::vector<int> split1;
        split1.reserve(mid);
        for (int j = 0; j < mid; ++j) {
            split1.push_back(j);
        }

        std::vector<int> split2;
        split2.reserve(fieldcount - mid);
        for (int j = mid; j < fieldcount; ++j) {
            split2.push_back(j);
        }

        splits.push_back(std::move(split1));
        splits.push_back(std::move(split2));

        options.schemaDescriptors.push_back(std::make_shared<rocksdb::DistributorSchemaDescriptor>(
            codec, in_schema, splits));

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
