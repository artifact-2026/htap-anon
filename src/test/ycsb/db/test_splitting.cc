#include <cmath>
#include <queue>
#include <future>

#include "core/core_workload.h"
#include "test_splitting.h"
#include "lib/coding.h"
#include <nlohmann/json.hpp>

#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "transformer/distributor.h"

using namespace std;

namespace ycsbc {
    TestSplitting::TestSplitting(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        int num_splits = 2;

        rocksdb::Options options;
        ycsbc::DBHelper::SetOptions(options, false, props);

        std::string transformation_type = props.GetProperty("transformtype", "mynoop");

        rocksdb::TransformerData data;
        if (transformation_type == "mynoop") {
            options.transformers.push_back(new rocksdb::Mynooper());
            data = rocksdb::MynooperData();
        } else if (transformation_type == "split") {
            options.transformers.push_back(new rocksdb::Distributor());
            std::string inputType = props.GetProperty("inputdataformat", "protobuf");
            rocksdb::InputOutputDataType dtype = ycsbc::DBHelper::mapStringToDataType(inputType);
            data = rocksdb::DistributorData(num_splits, false, dtype);
        }
        mymBroker_ = std::make_unique<rocksdb::MymBroker>(dbname, !bootstrap, dbfilename, options, data);
    }

    /*
    * Read is for point query over all columns
    */
    int TestSplitting::Read(const std::string &table, const std::string &key, const std::set<int> *fields,
                      const std::string &req_dist, bool index_access, std::string &result)
    {
        mymBroker_->Read(key, fields, result);
        return 0;
    }

    int TestSplitting::Scan(const std::string &table, const std::string &begin_key,
                          const std::string &end_key, const std::set<int> *fields,
                          const std::string &req_dist, bool index_access,
                          std::vector<std::string> &result) 
    {
        mymBroker_->Scan(begin_key, 100, fields, result);
        return 0;
    }

    int TestSplitting::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        mymBroker_->Insert(key, values);
        return 0;
    }

    int TestSplitting::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int TestSplitting::Delete(const std::string &table, const std::string &key)
    {
        mymBroker_->Delete(key);
        return 0;
    }
}
