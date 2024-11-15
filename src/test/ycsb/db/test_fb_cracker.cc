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
        rocksdb::InputOutputDataType inputType = ycsbc::DBHelper::mapStringToDataType(props.GetProperty("inputdatatype", "PROTOBUF"));
        rocksdb::InputOutputDataType outputType = ycsbc::DBHelper::mapStringToDataType(props.GetProperty("outputdatatype", "FLATBUFFERS"));
        int columnDataType = utils::StrToInt(props.GetProperty("columndatatype", "1"));
        SetOptions(props, bootstrap, levels, fieldcount, inputType, outputType, columnDataType);
        int num_splits = 2;

        options_.transformers.push_back(new rocksdb::Distributor());
        options_.transformers.push_back(new rocksdb::Converter());
        
        std::vector<rocksdb::ColumnFamilyDescriptor> column_family_descriptors;
        GetColumnFamilyDescriptors(dbname, column_family_descriptors, num_splits);
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
            s = rocksdb_->AddTransformingDestinationCfds(dbname, true, true, false, false, num_splits);
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

            s = rocksdb_->AddTransformingDestinationCfds(dbname, true, true, false, false, num_splits);
            if (!s.ok()){
                std::cerr<<"Column family creation for crackfb ran into error "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }
        }
        BuildColumnFamilyHandles(column_family_descriptors, cf_handles);
    }

    /*
    * Read is for point query over all columns
    */
    int TestFBCracker::Read(const std::string &table, const std::string &key, const std::set<std::string> *fields,
                      const std::string &req_dist, bool index_access, std::string &result) 
    {
        rocksdb::Status s;
        result = "";
        int num_splits = 2;
        int leaf_splits = num_splits*num_splits*num_splits;

        if (fields == nullptr) {
            if (req_dist == "leastrecent") {
                for (int j = 0; j < leaf_splits; j++) {
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
                    group *= num_splits;
                    for (int j = 0; j < group; j++) {
                        std::string foundvalue = "";
                        s = rocksdb_->Get(rocksdb::ReadOptions(),
                                          cfhandles_[table+"_converted_cf_sys_cf_L"+std::to_string(i)+"_G"+std::to_string(j)],
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
                }
            }
        } else {
            if (req_dist == "leastrecent") {
                s = rocksdb_->Get(rocksdb::ReadOptions(),
                                  cfhandles_[table+"_converted_cf_sys_cf_L3_G0"],
                                  key, &result);
                if (result != "") {
                    return 0;
                }
            } else {
                s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table], key, &result);
                if (result != "") {
                    return 0;
                }
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
        int searched = 0;
        int num_splits = 2;
        int leaf_splits = num_splits*num_splits*num_splits;

        if (fields == nullptr) {
            if (req_dist == "leastrecent") {
                for (int i = 0; i < leaf_splits; i++) {
                    auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandles_[table+"_converted_cf_sys_cf_L3_G"+std::to_string(i)]);
                    it->Seek(begin_key);
                    searched = 0;
                    while (it->Valid() && searched < 25) {
                        result.push_back(it->value().ToString());
                        it->Next();
                        searched++;
                    }
                    if (searched == 0) {
                        break;
                    }
                }
                if (searched == 25) {
                    return 0;
                }
            } else {
                int group = 1;
                for (int i = 0; i < 4; i++) {
                    for (int j = 0; j < group; j++) {
                        std::string cfname = table;
                        if (i > 0) {
                            cfname += "_converted_cf_sys_cf_L"+std::to_string(i)+"_G"+std::to_string(j);
                        }
                        auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandles_[cfname]);
                        it->Seek(begin_key);
                        searched = 0;
                        while (it->Valid() && searched < 25) {
                            result.push_back(it->value().ToString());
                            it->Next();
                            searched++;
                        }
                        if (searched == 0) {
                            break;
                        }
                    }
                    if (searched >= 25) {
                        return 0;
                    }
                    group *= num_splits;
                }
            }
        } else {
            if (req_dist == "leastrecent") {
                auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandles_[table+"_converted_cf_sys_cf_L3_G0"]);
                it->Seek(begin_key);

                while (it->Valid() && searched < 25) {
                    result.push_back(it->value().ToString());
                    it->Next();
                    searched++;
                }
                if (searched == 25) {
                    return 0;
                }
            } else {
                std::set<std::string> keyset;
                for (int i = 0; i < 4; i++) {
                    std::string tablename;
                    if (i > 0) {
                        tablename = table + "_converted_cf_sys_cf_L" + std::to_string(i) + "_G0";
                    } else {
                        tablename = table;
                    }

                    auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandles_[tablename]);
                    it->Seek(begin_key);

                    while (it->Valid() && searched < 25) {
                        result.push_back(it->value().ToString());
                    
                        it->Next();
                        searched++;
                    }
                    if (searched >= 25) {
                        return 0;
                    }
                }
            }
        }
       
        return 1;
    }

    int TestFBCracker::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        rocksdb::Status s = rocksdb_->Put(write_options_,
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
        rocksdb::Status s = rocksdb_->Delete(write_options_,
                                             cfhandles_[table],
                                             key);
        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    void TestFBCracker::SetOptions(utils::Properties &props, bool logging, int levels, int fieldcount,
                                rocksdb::InputOutputDataType inputDataType, rocksdb::InputOutputDataType outputDataType,
                                int columndatatype)
    {
        if (!logging) {
            options_.info_log_level = rocksdb::InfoLogLevel::FATAL_LEVEL;
        }
        
        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;
        options_.max_open_files = -1;

        options_.num_levels = levels;
        options_.num_columns = fieldcount;
        options_.column_data_type = columndatatype;
        options_.SetTransformerType(rocksdb::TransformerType::DISTRIBUTOR);
        options_.SetInputOutputDataType(inputDataType, outputDataType);

        options_.write_buffer_size = 256 * 1024 * 1024;
        options_.max_write_buffer_number = 4;
        options_.level0_file_num_compaction_trigger = 10;
        options_.level0_slowdown_writes_trigger = 20;
        options_.level0_stop_writes_trigger = 36;
        options_.max_background_flushes = 2;
        options_.max_background_compactions = 10;
        options_.compression = rocksdb::kNoCompression;
        options_.table_factory.reset(rocksdb::NewBlockBasedTableFactory(
                rocksdb::BlockBasedTableOptions{
                .block_cache = rocksdb::NewLRUCache(512 * 1024 * 1024)}));

        write_options_.disableWAL = true;

        /*
        options_.IncreaseParallelism(16);
        options_.target_file_size_base = 67108864;
        options_.use_direct_reads = true;
        options_.use_direct_io_for_flush_and_compaction = true;
        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_cache = nullptr;  // Disable the block cache
        options_.table_factory = std::shared_ptr<rocksdb::TableFactory>(rocksdb::NewBlockBasedTableFactory(table_options));
        */
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
                                             std::vector<rocksdb::ColumnFamilyDescriptor> &column_families,
                                             int num_splits)
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

                for (int k = 0; k < num_splits; k++) {
                    int child = parent_cols/(num_splits-k);
                    std::string cfname1 = prefix + "_L" + std::to_string(level) + "_G" + std::to_string(j*num_splits+k);
                    column_families.push_back(rocksdb::ColumnFamilyDescriptor(cfname1, rocksdb::ColumnFamilyOptions(options_)));
                    parents.push(child);

                    if (k < num_splits-1) {
                        parent_cols -= child;
                        if (parent_cols == 0) {
                            break;
                        }
                    }
                }
            }
        }
    }

}
