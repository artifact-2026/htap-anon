#ifndef CABINDB_COLUMNFAMILIES_H
#define CABINDB_COLUMNFAMILIES_H

#include <vector>
#include <set>
#include "cabindb_namespace.h"

namespace rocksdb {
    class ColumnFamilyDescriptor;
    class ColumnFamilyHandle;
}

namespace CABINDB_NAMESPACE {

  void CreateAllColumnFamilyDescriptors(int field_count, int num_levels,
                    std::vector<rocksdb::ColumnFamilyDescriptor> &descriptors);
  void PopulateColumnGroups(std::vector<std::set<std::string> > &column_groups,
                    int num_levels, int fieldcount);

  inline void CreateAllColumnFamilyDescriptors(int field_count, int num_levels,
                        std::vector<rocksdb::ColumnFamilyDescriptor> &descriptors) {
    descriptors.push_back(rocksdb::ColumnFamilyDescriptor(rocksdb::kDefaultColumnFamilyName,
			    rocksdb::ColumnFamilyOptions()));

    for (int i = 1; i < num_levels; i++) {
      if (i == num_levels - 1) {
        for (int j = 0; j < field_count; j++) {
          descriptors.push_back(rocksdb::ColumnFamilyDescriptor(
			    "cf_" + std::to_string(i) + "_" + std::to_string(j), 
			    rocksdb::ColumnFamilyOptions()));
        }
      } else {
	for (int j = 0; j < pow(2, i); j++) {
	  descriptors.push_back(rocksdb::ColumnFamilyDescriptor(
			    "cf_" + std::to_string(i) + "_" + std::to_string(j),
			    rocksdb::ColumnFamilyOptions()));
	}
      } 
    }	    
  }

  inline void PopulateColumnGroups(std::vector<std::set<std::string> > &column_groups,
                    int num_levels, int fieldcount) {
    for (int i = 0; i < num_levels; i++) {
      int group_num = i == num_levels -1 ? fieldcount : pow(2, i);
      int group_size = fieldcount / group_num;

      for (int j = 0; j < group_num; j++) {
        std::set<std::string> columns;
        for (int k = 0; k < group_size; k++) {
          columns.insert("field"+std::to_string(j*group_size+k));
        }

        if (j == group_num -1 && group_size * group_num < fieldcount) {
          for (int k = group_size * group_num; k < fieldcount; k++) {
            columns.insert("field"+std::to_string(k));
          }
        }
        column_groups.push_back(columns);
      }
    }
  }

}

#endif
