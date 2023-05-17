#include "rocksdb/db.h"
#include "column_families.h"

namespace CABINDB_NAMESPACE {

    ColumnFamilies::ColumnFamilies(int num_levels, int field_count) {

        // level-0 column family
        cfdescriptors_.push_back(rocksdb::ColumnFamilyDescriptor(
            rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions()));

        // level-1 to second to last leaf level column families
        for (int i = 1; i < num_levels - 1; i++) {
            CreateInternalColumnFamilyDescriptor(i);
        }
        
        // leaf level column families
        CreateLeafLevelColumnFamilyDescriptor(num_levels-1, field_count);
    }

    std::vector<rocksdb::ColumnFamilyDescriptor> ColumnFamilies::GetColumnFamilyDescriptors() {
        return cfdescriptors_;
    }

    std::vector<rocksdb::ColumnFamilyHandle*>* ColumnFamilies::GetColumnFamilyHandles() {
        return cfhandles_;
    }

    void ColumnFamilies::CreateInternalColumnFamilyDescriptor(int level) {
        for (int i = 0; i < level*2; i++) {
            cfdescriptors_.push_back(rocksdb::ColumnFamilyDescriptor(
                "level_"+std::to_string(level)+"_cf_"+std::to_string(i+1), rocksdb::ColumnFamilyOptions()));
        }
    }

    void ColumnFamilies::CreateLeafLevelColumnFamilyDescriptor(int leaflevel, int fieldcount) {
        for (int i = 0; i < fieldcount; i++) {
            cfdescriptors_.push_back(rocksdb::ColumnFamilyDescriptor(
                "level_"+std::to_string(leaflevel)+"_cf_"+std::to_string(i+1), rocksdb::ColumnFamilyOptions()));
        }
    }

} // CABINDB_NAMESPACE