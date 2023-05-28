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

  void CreateLeveledColumnFamilyNames(int field_count, int num_levels,
                    std::vector<std::vector<std::string> > &leveled_cf_names);
  void CreateAllColumnFamilyDescriptors(std::vector<rocksdb::ColumnFamilyDescriptor> &descriptors,
		                std::vector<std::vector<std::string> > &leveled_cf_names);
  void PopulateColumnGroups(std::vector<std::set<std::string> > &column_groups,
                    int num_levels, int fieldcount);

  inline void CreateLeveledColumnFamilyNames(int field_count, int num_levels,
                    std::vector<std::vector<std::string> > &leveled_cf_names) {
    std::vector<std::string> level_0_cf_names;
    level_0_cf_names.push_back(rocksdb::kDefaultColumnFamilyName);
    leveled_cf_names.push_back(level_0_cf_names);

    for (int i = 1; i < num_levels; i++) {
      int column_groups;
      if (i == num_levels - 1) {
 	      column_groups = field_count;
      } else {
	      column_groups = pow(2, i);
      }

      std::vector<std::string> level_i_cf_names;
      for (int j = 0; j < column_groups; j++) {
        level_i_cf_names.push_back("cf_" + std::to_string(i) + "_" + std::to_string(j));
      }
      leveled_cf_names.push_back(level_i_cf_names);
    }
  }

  inline void CreateAllColumnFamilyDescriptors(
                        std::vector<rocksdb::ColumnFamilyDescriptor> &descriptors,
			                  std::vector<std::vector<std::string> > &leveled_cf_names)
  {
    for (auto cf_names : leveled_cf_names) {
      for (auto name : cf_names) {
        descriptors.push_back(rocksdb::ColumnFamilyDescriptor(name,
                                          rocksdb::ColumnFamilyOptions()));
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
