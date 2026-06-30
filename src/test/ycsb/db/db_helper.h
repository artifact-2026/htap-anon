#ifndef YCSB_CPLUSPLUS_DBHELPER_H
#define YCSB_CPLUSPLUS_DBHELPER_H

#include <rocksdb/options.h>
#include "core/properties.h"
#include "mycelium/transformer.h"

namespace ycsbc {

class DBHelper {
    public:
        static mycelium::InputOutputDataType mapStringToDataType(const std::string& dataType);
        static void SetOptions(rocksdb::Options& options_, bool logging, utils::Properties &props);
};  

}

#endif
