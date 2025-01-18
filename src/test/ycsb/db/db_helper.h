#ifndef YCSB_CPLUSPLUS_DBHELPER_H
#define YCSB_CPLUSPLUS_DBHELPER_H

#include <rocksdb/options.h>
#include "rocksdb/transformer.h"

namespace ycsbc {

class DBHelper {
    public:
        static rocksdb::InputOutputDataType mapStringToDataType(const std::string& dataType);
        static void SetOptions(rocksdb::Options& options_, const char *dbfilename, bool logging, 
                        int levels, int fieldcount);
};  

}

#endif
