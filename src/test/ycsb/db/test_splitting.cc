#include "core/core_workload.h"
#include "test_splitting.h"
#include "transformer/distributor.h"

using namespace std;

namespace ycsbc {
    TestSplitting::TestSplitting(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));

        rocksdb::Options options;
        ycsbc::DBHelper::SetOptions(options, false, props);

        std::string inputType = props.GetProperty("inputdataformat", "protobuf");

        options.transformers.push_back(new rocksdb::Distributor());
        options.SetTransformerType(rocksdb::TransformerType::DISTRIBUTOR);

        auto input_proto = std::make_unique<data::Row>();
        std::vector<std::unique_ptr<google::protobuf::Message>> output_protos;
        output_protos.emplace_back(std::make_unique<data::Grp1>());
        output_protos.emplace_back(std::make_unique<data::Grp2>());
        output_protos.emplace_back(std::make_unique<data::Grp3>());
        output_protos.emplace_back(std::make_unique<data::Grp4>());
        rocksdb::ProtobufDistributorSchema schema = rocksdb::ProtobufDistributorSchema(std::move(input_proto), std::move(output_protos));
        mymBroker_ = std::make_unique<rocksdb::MymBroker>(dbname, !bootstrap, dbfilename, options, schema);
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
