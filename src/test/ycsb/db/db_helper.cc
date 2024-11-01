#include "db_helper.h"

using namespace std;

namespace ycsbc {
    
    rocksdb::InputOutputDataType DBHelper::mapStringToDataType(const std::string& dataType) {
        if (dataType == "JSON") return rocksdb::InputOutputDataType::JSON;
        if (dataType == "PROTOBUF") return rocksdb::InputOutputDataType::PROTOBUF;
        if (dataType == "FLATBUFFERS") return rocksdb::InputOutputDataType::FLATBUFFERS;
        return rocksdb::InputOutputDataType::UNKNOWN;
    }

}
