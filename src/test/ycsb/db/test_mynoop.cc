#include <nlohmann/json.hpp>
#include "core/core_workload.h"
#include "test_mynoop.h"
#include "lib/coding.h"
#include "transformer/identity/mynooper.h"

using namespace std;

namespace ycsbc {
    TestMynoop::TestMynoop(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        
        rocksdb::Options options;
        ycsbc::DBHelper::SetOptions(options, true, props);
        options.transformers.push_back(std::make_shared<rocksdb::Mynooper>());
        options.schemaDescriptors.push_back(std::make_shared<rocksdb::MynooperSchema>());

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
