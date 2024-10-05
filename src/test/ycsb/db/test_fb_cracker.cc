#include <queue>
#include <iostream>
#include "core/core_workload.h"
#include "lib/coding.h"
#include "test_fb_cracker.h"
#include "transformer/converter.h"
#include "transformer/distributor.h"

using namespace std;

namespace ycsbc {
    TestFBCracker::TestFBCracker(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        int levels = utils::StrToInt(props.GetProperty("levels", "6"));
        int fieldcount = utils::StrToInt(props.GetProperty("fieldcount", "1"));
        noResults = 0;
        SetOptions(props, bootstrap, levels, fieldcount);

        options_.transformers.push_back(new rocksdb::Distributor());
        options_.transformers.push_back(new rocksdb::Converter());
        
        std::vector<rocksdb::ColumnFamilyDescriptor> column_family_descriptors;
        GetColumnFamilyDescriptors(dbname, column_family_descriptors);
        std::vector<rocksdb::ColumnFamilyHandle*> cf_handles;

        if (bootstrap) {
            rocksdb::Status s = rocksdb::DB::Open(options_, 
                                              dbfilename,
                                              &rocksdb_);
            if (!s.ok()){
                std::cerr<<"Can't open flat cracker "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }

            s = rocksdb_->CreateColumnFamilies(column_family_descriptors, &cf_handles);
            s = rocksdb_->AddTransformingDestinationCfds(dbname, true, true, false);
            if (!s.ok()) {
                std::cerr<<"Creating column families ran into error: "<<s.ToString()<<std::endl;
                exit(0);
            }
        } else {
            column_family_descriptors.push_back(rocksdb::ColumnFamilyDescriptor(
                    rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions(options_)));
            rocksdb::Status s = rocksdb::DB::Open(options_,
                                              dbfilename,
                                              column_family_descriptors,
                                              &cf_handles,
                                              &rocksdb_);

            if (!s.ok()){
                std::cerr<<"Can't open flat cracker "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }

            s = rocksdb_->AddTransformingDestinationCfds(dbname, true, true, false);
            if (!s.ok()){
                std::cerr<<"Column family creation for crackfb ran into error "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }
        }
        BuildColumnFamilyHandles(column_family_descriptors, cf_handles);
        rocksdb_->DisplayTransformingDestinationCfds();
    }

    /*
    * Read is for point query over all columns
    */
    int TestFBCracker::Read(const std::string &table, const std::string &key, const std::set<std::string> *fields,
                      const std::string &req_dist, bool index_access, std::string &result) 
    {
        rocksdb::Status s;

        if (fields == nullptr) {
            if (req_dist == "earliest") {
                for (int j = 0; j < 8; j++) {
                    std::string foundvalue;
                    s = rocksdb_->Get(rocksdb::ReadOptions(),
                                  cfhandles_[table+"_converted_cf_sys_cf_L3_G"+std::to_string(j)],
                                  key, &foundvalue);
                    if (foundvalue != "") {
                        result += foundvalue;
                    } else {
                        break;
                    }
                }
                if (result != "") {
                    return 0;
                }
            } else {
                s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table], key, &result);
                if (result != "") {
                    return 0;
                }

                int group = 1;
                for (int i = 1; i < 4; i++) {
                    group *= 2;
                    for (int j = 0; j < group; j++) {
                        std::string foundvalue;
                        s = rocksdb_->Get(rocksdb::ReadOptions(),
                                          cfhandles_[table+"_converted_cf_sys_cf_L"+std::to_string(i)+"_G"+std::to_string(j)],
                                          key, &foundvalue);
                        if (s.ok() && foundvalue != "") {
                            result += foundvalue;
                        } else {
                            break;
                        }
                    }
                
                    if (result != "") {
                        return 0;
                    }
                }
            }
        } else {
            if (req_dist == "earliest") {
                s = rocksdb_->Get(rocksdb::ReadOptions(),
                                  cfhandles_[table+"_converted_cf_sys_cf_L3_G0"],
                                  key, &result);
                if (result != "") {
                    return 0;
                }
            } else {
                for (int i = 1; i < 4; i++) {
                    s = rocksdb_->Get(rocksdb::ReadOptions(),
                                      cfhandles_[table+"_converted_cf_sys_cf_L"+std::to_string(i)+"_G0"],
                                      key, &result);
                    if (result != "") {
                        return 0;
                    }
                }
            }
        }
        return 1;
    }

    int TestFBCracker::Scan(const std::string &table, const std::string &begin_key,
                          const std::string &end_key, const std::set<std::string> *fields,
                          const std::string &req_dist, bool index_access,
                          std::vector<std::string> &result) 
    {
        std::set<std::string> values;

        for (int i = 3; i >= 0; i--) {
            std::string tablename;
            if (i > 0) {
                tablename = table + "_converted_cf_sys_cf_L" + std::to_string(i) + "_G0";
            } else {
                tablename = table;
            }

            auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandles_[tablename]);
            it->Seek(begin_key);

            while (it->Valid()) {
                if (it->key().ToString() < end_key) {
                    values.insert(it->value().ToString());
                } else {
                    break;
                }
                it->Next();
            }
        }
        return values.size();
    }

    int TestFBCracker::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        rocksdb::Status s = rocksdb_->Put(rocksdb::WriteOptions(),
                                          cfhandles_[table],
                                          key,
                                          values);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    int TestFBCracker::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int TestFBCracker::Delete(const std::string &table, const std::string &key)
    {
        rocksdb::Status s = rocksdb_->Delete(rocksdb::WriteOptions(),
                                             cfhandles_[table],
                                             key);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    void TestFBCracker::SetOptions(utils::Properties &props, bool logging, int levels, int fieldcount)
    {
        if (!logging) {
            options_.info_log_level = rocksdb::InfoLogLevel::FATAL_LEVEL;
        }
        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;

        options_.num_levels = levels;
        options_.num_columns = fieldcount;
        options_.SetTransformerType(rocksdb::TransformerType::DISTRIBUTOR);

        options_.IncreaseParallelism(16);
        options_.level0_slowdown_writes_trigger = 16;     
        options_.level0_stop_writes_trigger = 24;
        options_.max_open_files = -1;
        options_.level0_file_num_compaction_trigger = 8;

        options_.max_write_buffer_number = 3;
        options_.write_buffer_size = 67108864;
        options_.target_file_size_base = 67108864;

        options_.use_direct_reads = true;
        options_.use_direct_io_for_flush_and_compaction = true;

        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_cache = nullptr;  // Disable the block cache
        options_.table_factory = std::shared_ptr<rocksdb::TableFactory>(rocksdb::NewBlockBasedTableFactory(table_options));
    }

    void TestFBCracker::KeepOnlyRequestedFields(data::Row &row,
                    const std::set<std::string> *fields, data::Row &selectedColumns)
    {
        for (auto field : *fields) {
            for (int i = 0; i < row.columns_size(); i++) {
                if (row.columns(i).name().compare(field) == 0) {
                    data::Column* selectedColumn = selectedColumns.add_columns();
                    selectedColumn->set_name(row.columns(i).name());
                    selectedColumn->set_value(row.columns(i).value());
                    break;
                }
            }
        }
    }

    void TestFBCracker::BuildColumnFamilyHandles(std::vector<rocksdb::ColumnFamilyDescriptor> &column_family_descriptors,
                                                std::vector<rocksdb::ColumnFamilyHandle *> handles)
    {
        for (size_t i = 0; i < handles.size(); i++) {
            if (column_family_descriptors[i].name != rocksdb::kDefaultColumnFamilyName) {
                cfhandles_.insert({column_family_descriptors[i].name, handles[i]});
                cfhandlelist_.push_back(handles[i]);
            }
        }
    }

    void TestFBCracker::BuildQueryHandles(std::set<std::string> fields) {
        std::set<int> fieldpositions;
        for (auto field : fields) {
            int pos = 0;
            for (size_t i=5; i < field.size(); i++) {
                if (!isdigit(field[i])) {
                    break;
                }
                pos = pos*10 + field[i] - '0';
            }
            fieldpositions.insert(pos);
        }

        int columns = options_.num_columns;
        int splits = 1;
        std::map<int, std::set<int>> leveled_positions;
        for (int level=1; level < 4; level++) {
            splits *= 2;
            columns /= 2;
            std::set<int> probes;
            for (auto pos : fieldpositions) {
                probes.insert(pos/columns);
            }
            leveled_positions.insert({level, probes});
        }

        std::vector<rocksdb::ColumnFamilyHandle *> level0hdls;
        level0hdls.push_back(cfhandlelist_[0]);
        cached_cfhandles_.insert({0, level0hdls});

        int index = 1;
        int sz = 1;
        for (int level = 1; level < 4; level++) {
            std::vector<rocksdb::ColumnFamilyHandle *> levelhdls;

            auto it = leveled_positions.find(level);
            if (it != leveled_positions.end()) {
                for (auto pos : it->second) {
                    levelhdls.push_back(cfhandlelist_[index+pos]);
                }
                cached_cfhandles_.insert({level, levelhdls});
            }
            sz *= 2;
            index += sz;
        }
    }

    void TestFBCracker::GetColumnFamilyDescriptors(const std::string &dbname,
                                             std::vector<rocksdb::ColumnFamilyDescriptor> &column_families)
    {
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(
            dbname, rocksdb::ColumnFamilyOptions(options_)));
      
        std::string prefix = dbname + "_converted_cf_sys_cf";
        std::queue<int> parents;
        parents.push(options_.num_columns);

        int total_levels = options_.num_levels;
        for (int level = 1; level < total_levels - 2; level++) {
            int queueLen = parents.size();

            options_.num_levels -= level;
            if (level == total_levels - 3) {
                options_.SetTransformerType(rocksdb::TransformerType::NOTRANSFORMATION);
            } else if (level == total_levels - 4) {
                options_.SetTransformerType(rocksdb::TransformerType::DISTRIBUTOR | rocksdb::TransformerType::CONVERTER);
            }
            for (int j = 0; j < queueLen; j++) {
                int parent_cols = parents.front();
                parents.pop();
                if (parent_cols < 2) {
                    continue;
                }

                int child1 = parent_cols/2;
                std::string cfname1 = prefix + "_L" + std::to_string(level) + "_G" + std::to_string(j*2);
                column_families.push_back(rocksdb::ColumnFamilyDescriptor(cfname1, rocksdb::ColumnFamilyOptions(options_)));
                parents.push(child1);

                int child2 = parent_cols - child1;
                std::string cfname2 = prefix + "_L" + std::to_string(level) + "_G" + std::to_string(j*2+1);
                column_families.push_back(rocksdb::ColumnFamilyDescriptor(cfname2, rocksdb::ColumnFamilyOptions(options_)));
                parents.push(child2);
            }
        }
    }

}
