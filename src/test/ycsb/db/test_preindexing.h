#ifndef YCSB_CPLUSPLUS_PREINDEXING_H
#define YCSB_CPLUSPLUS_PREINDEXING_H

#include "core/db.h"

#include <iostream>
#include <sstream>
#include <errno.h>
#include <string>

#include <rocksdb/options.h>
#include <rocksdb/db.h>
#include <rocksdb/cache.h>
#include <rocksdb/merge_operator.h>
#include <rocksdb/table.h>
#include <rocksdb/filter_policy.h>

#include "core/properties.h"
#include "core/core_workload.h"
#include "proto/columns.pb.h"

#include "db/db_helper.h"

namespace ycsbc {

class SecondaryPreindexMergeOperator : public rocksdb::AssociativeMergeOperator {
public:
    bool Merge(const rocksdb::Slice& key, 
               const rocksdb::Slice* existing_value,
               const rocksdb::Slice& value,
               std::string* new_value,
               rocksdb::Logger* logger) const override {
        // If there's an existing value, start with it; otherwise, initialize as empty
        std::string merged_value = existing_value ? existing_value->ToString() : "";

        // Convert the new primary key to a string
        std::string new_primary_key = value.ToString();

        // If deduplication is needed
        if (merged_value.find(new_primary_key) == std::string::npos) {
            if (!merged_value.empty()) {
                merged_value += ",";  // Add a separator before appending
            }
            merged_value += new_primary_key;  // Append the new primary key
        }

        // Return the merged result
        *new_value = merged_value;
        return true;
    }

    const char* Name() const override { return "SecondaryPreindexMergeOperator"; }
};

class TestPreindexing : public DB{
    public :
        TestPreindexing(const std::string& dbname, const char *dbfilename, utils::Properties &props);
        int Read(const std::string &table, const std::string &key,
                 const std::set<std::string> *fields,
                 const std::string &req_dist, bool index_access,
                 std::string &result);

        int Scan(const std::string &table, const std::string &begin_key,
                 const std::string &end_key, const std::set<std::string> *fields,
                 const std::string &req_dist, bool index_access,
                 std::vector<std::string> &result);

        int Insert(const std::string &table, const std::string &key,
                   std::string &values);

        int Update(const std::string &table, const std::string &key,
                   std::string &values);

        int Delete(const std::string &table, const std::string &key);

        ~TestPreindexing() {};
    
    private:
        rocksdb::DB *rocksdb_;
        rocksdb::Options options_;
        rocksdb::WriteOptions write_options_;
        int noResults;
        std::map<std::string, rocksdb::ColumnFamilyHandle*> cfhandles_;

        void SetOptions(const char *dbfilename, int levels, int fieldcount, bool logging,
                        rocksdb::InputOutputDataType inputType,
                        rocksdb::InputOutputDataType outputType);
        void GetColumnFamilyDescriptors(const std::string& dbname,
                    std::vector<rocksdb::ColumnFamilyDescriptor>& column_families);
        void BuildColumnFamilyHandleMap(std::vector<rocksdb::ColumnFamilyDescriptor>& column_family_descriptors,
                            std::vector<rocksdb::ColumnFamilyHandle*> handles);
        std::set<int> GetQueryingHandles(std::set<std::string> fields);
        std::vector<std::string> deserializeIndex(const std::string& serialized);
        std::vector<std::string> parsePrimaryKeys(const std::string& value);
        void removePrimaryKeyFromList(std::string& value, const std::string& pkey);
};  

}

#endif
