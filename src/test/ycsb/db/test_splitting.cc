#include "core/core_workload.h"
#include "test_splitting.h"
#include "transformer/distribute/distributor.h"

using namespace std;

namespace ycsbc {
    TestSplitting::TestSplitting(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));

        rocksdb::Options options;
        ycsbc::DBHelper::SetOptions(options, true, props);

        options.transformers.push_back(std::make_shared<rocksdb::Distributor>());
        options.transformers.push_back(std::make_shared<rocksdb::Distributor>());

        auto input_proto = std::make_unique<data::ByteRow>();
        std::vector<std::unique_ptr<google::protobuf::Message>> output_protos;
        output_protos.emplace_back(std::make_unique<data::ByteRow>());
        output_protos.emplace_back(std::make_unique<data::ByteRow>());
        output_protos.emplace_back(std::make_unique<data::ByteRow>());
        output_protos.emplace_back(std::make_unique<data::ByteRow>());
        options.schemaDescriptors.push_back(std::make_shared<rocksdb::ProtobufDistributorSchema>(
            output_protos.size(), std::move(input_proto), std::move(output_protos)));

        auto input_proto1 = std::make_unique<data::ByteRow>();
        std::vector<std::unique_ptr<google::protobuf::Message>> output_protos1;
        output_protos1.emplace_back(std::make_unique<data::ByteRow>());
        output_protos1.emplace_back(std::make_unique<data::ByteRow>());
        output_protos1.emplace_back(std::make_unique<data::ByteRow>());
        output_protos1.emplace_back(std::make_unique<data::ByteRow>());
        options.schemaDescriptors.push_back(std::make_shared<rocksdb::ProtobufDistributorSchema>(
            output_protos1.size(), std::move(input_proto1), std::move(output_protos1)));
        
        

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
