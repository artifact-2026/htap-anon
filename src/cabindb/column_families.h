#ifndef CABINDB_COLUMNFAMILIES_H
#define CABINDB_COLUMNFAMILIES_H

#include <vector>
#include "cabindb_namespace.h"

namespace rocksdb {
    class ColumnFamilyDescriptor;
    class ColumnFamilyHandle;
}

namespace CABINDB_NAMESPACE {

class ColumnFamilies {
  public:
    ColumnFamilies(int num_levels, int field_count);

    std::vector<rocksdb::ColumnFamilyDescriptor> GetColumnFamilyDescriptors();

    std::vector<rocksdb::ColumnFamilyHandle*>* GetColumnFamilyHandles();

    ~ColumnFamilies();

  private:
    std::vector<rocksdb::ColumnFamilyDescriptor> cfdescriptors_;
    std::vector<rocksdb::ColumnFamilyHandle*>* cfhandles_;

    void CreateInternalColumnFamilyDescriptor(int level);
    void CreateLeafLevelColumnFamilyDescriptor(int leaflevel, int fieldcount);
};

}

#endif