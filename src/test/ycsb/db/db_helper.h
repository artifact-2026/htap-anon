#ifndef YCSB_CPLUSPLUS_DBHELPER_H
#define YCSB_CPLUSPLUS_DBHELPER_H

#include <iostream>
#include "rocksdb/transformer.h"

namespace ycsbc {

class DBHelper {
    public:
        static rocksdb::InputOutputDataType mapStringToDataType(const std::string& dataType);
};  

}

#endif
