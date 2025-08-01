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

using namespace std;

namespace ycsbc {
    TestFbCracker::TestFbCracker(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        
        rocksdb::Options options;
        ycsbc::DBHelper::SetOptions(options, false, props);
        
        options.transformers.push_back(std::make_shared<rocksdb::Distributor>());
        options.transformers.push_back(std::make_shared<rocksdb::Converter>());

        auto input_proto = std::make_unique<data::Row>();
        std::vector<std::unique_ptr<google::protobuf::Message>> output_protos;
        output_protos.emplace_back(std::make_unique<data::Grp1>());
        output_protos.emplace_back(std::make_unique<data::Grp2>());
        output_protos.emplace_back(std::make_unique<data::Grp3>());
        output_protos.emplace_back(std::make_unique<data::Grp4>());
        options.schemaDescriptors.push_back(
                std::make_shared<rocksdb::ProtobufDistributorSchema>(2, 
                    std::move(input_proto), std::move(output_protos)));

        std::unique_ptr<google::protobuf::Message> input_proto_template = std::make_unique<data::Row>();
        const flatbuffers::TypeTable* fb_type_table = flat::FbRowTypeTable();
        options.schemaDescriptors.push_back(
                std::make_shared<rocksdb::Protobuf2FlatbuffersSchema>(
                    std::move(input_proto_template), fb_type_table));

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
