#include "core/core_workload.h"
#include "test_rocks_db.h"
#include "lib/coding.h"
#include "test_converting.h"
#include "transformer/converter.h"

#include <iostream>
#include <iomanip> // Include for std::setfill and std::setw

using namespace std;

namespace ycsbc {
    TestFlatBuffers::TestFlatBuffers(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        
        rocksdb::InputOutputDataType inputType = ycsbc::DBHelper::mapStringToDataType(props.GetProperty("inputdataformat", "PROTOBUF"));
        rocksdb::InputOutputDataType outputType = ycsbc::DBHelper::mapStringToDataType(props.GetProperty("outputdataformat", "FLATBUFFERS"));
        std::string columnDataType = props.GetProperty("columndatatype", "numeric");

        rocksdb::Options options;
        ycsbc::DBHelper::SetOptions(options, false, props);
        
        options.transformers.push_back(new rocksdb::Converter());
        options.SetTransformerType(rocksdb::TransformerType::CONVERTER);
        rocksdb::ConverterData data = rocksdb::ConverterData(inputType, outputType, columnDataType);
        mymBroker_ = std::make_unique<rocksdb::MymBroker>(dbname, !bootstrap, dbfilename, options, data); 
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
