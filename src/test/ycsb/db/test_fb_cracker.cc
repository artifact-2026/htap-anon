#include <queue>
#include "core/core_workload.h"
#include "test_rocks_db.h"
#include "lib/coding.h"
#include "cabindb/compactor.h"
#include "test_fb_cracker.h"
#include "lib/rocksdb/transformer/bytecracker.h"

using namespace std;

namespace ycsbc {
    TestFBCracker::TestFBCracker(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","true"));
        bool transform = utils::StrToBool(props.GetProperty("transform","true"));
        std::string translevel = props.GetProperty("translevel","all");
        noResults = 0;
        SetOptions(props);

        if (transform) {
            options_.transformer = std::make_shared<rocksdb::Bytecracker>();
        }

        std::vector<rocksdb::ColumnFamilyDescriptor> column_family_descriptors;
        GetColumnFamilyDescriptors(dbname, column_family_descriptors, translevel);
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
        }
        BuildColumnFamilyHandles(column_family_descriptors, cf_handles);
    }

    /*
    * Read is for point query over all columns
    */
    int TestFBCracker::Read(const std::string &table, const std::string &key, const std::set<std::string> *fields,
                      std::string &result) 
    {
        if (cached_cfhandles_.size() == 0) {
            if (fields != nullptr) {
                BuildQueryHandles(std::set<std::string>(fields->begin(), fields->end()));
            } else {
                int index = 0;
                int sz = 1;
                for (int level = 0; level < 4; level++) {
                    std::vector<rocksdb::ColumnFamilyHandle *> levelhdls;

                    for (int i = 0; i < sz; i++) {
                        levelhdls.push_back(cfhandlelist_[index+i]);
                    }
                    index += sz;
                    sz *= 2;
                    cached_cfhandles_.insert({level, levelhdls});
                }
            }
        }

        std::string rawResult; 
        for (int level = 0; level < 4; level++) {
            auto it = cached_cfhandles_.find(level);
            if (it != cached_cfhandles_.end()) {
                for (auto hdl : it->second) {
                    std::string value;
                    rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(),
                                              hdl,
                                              key,
                                              &value);
                    if (value.empty()) {
                        break;
                    }
                    rawResult += value;
                }
            }
            if (!rawResult.empty()) {
                if (level < 3) {
                    data::Row row;
                    row.ParseFromString(rawResult);
                    size_t fieldsFound = 0;
                    for (int i = 0; i < row.columns_size(); i++) {
                        if (fields == nullptr || fields->find(row.columns(i).name()) != fields->end()) {
                            result += row.columns(i).name() + "::" + row.columns(i).value() + ",";
                            fieldsFound++;
                            if (fields != nullptr && fieldsFound >= fields->size()) {
                                break;
                            }
                        }
                    }
                } else {
                    std::vector<uint8_t> vec(rawResult.begin(), rawResult.end());
                    flatbuffers::Verifier verifier(vec.data(), vec.size());
                    const FbRow* fbRow = GetFbRow(vec.data());
                    size_t fieldsFound = 0;

                    if (fbRow != nullptr && fbRow->Verify(verifier)) {
                        const flatbuffers::Vector<flatbuffers::Offset<NumericColumn>>* numcols = fbRow->numcols();
                        if (numcols != nullptr) {
                            for (size_t i = 0; i < numcols->size(); i++) {
                                if (fields == nullptr || fields->find(numcols->Get(i)->name()->str()) != fields->end()) {
                                    result += numcols->Get(i)->name()->str() + "::" + std::to_string(numcols->Get(i)->value());
                                    fieldsFound++;
                                    if (fields != nullptr && fieldsFound >= fields->size()) {
                                        break;
                                    }
                                }
                            }    
                        }
                    }
                }
                break;
            }
        }

        return 0;
    }

    int TestFBCracker::Scan(const std::string &table, const std::string &begin_key,
                          int32_t len, const std::set<std::string> *fields,
                          std::vector<std::string> &result) 
    {
        if (cached_cfhandles_.size() == 0) {
            BuildQueryHandles(std::set<std::string>(fields->begin(), fields->end()));
        }

        int32_t remaining_len = len;
        int32_t level_len = 0;
        for (int level = 0; level < 4; level++) {
            level_len = remaining_len/(4 - level);
            remaining_len -= level_len;
            if (level_len < 1) {
                continue;
            }

            auto it = cached_cfhandles_.find(level);
            if (it != cached_cfhandles_.end()) {
                if (it->second.size() > 0) {
                    auto itt = rocksdb_->NewIterator(rocksdb::ReadOptions(), it->second[0]);
                    itt->Seek(begin_key);
                    for (int i = 0; i < level_len && itt->Valid(); i++) {
                        std::string value = itt->value().ToString();
                        result.push_back(value);
                        itt->Next();
                    }
                }
            }
        }

        return result.size();
    }

    int TestFBCracker::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        auto it = cfhandles_.find(table);
        if (it != cfhandles_.end())
        {
            rocksdb::Status s = rocksdb_->Put(rocksdb::WriteOptions(),
                                              it->second,
                                              key,
                                              values);
            if (s.ok())
            {
                return 0;
            }
        }
        return 1;
    }

    int TestFBCracker::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int TestFBCracker::Delete(const std::string &table, const std::string &key)
    {
        auto it = cfhandles_.find(table);
        if (it != cfhandles_.end())
        {
            rocksdb::Status s = rocksdb_->Delete(rocksdb::WriteOptions(),
                                                 it->second,
                                                 key);
            if (s.ok())
            {
                return 0;
            }
        }
        return 1;
    }

    void TestFBCracker::SetOptions(utils::Properties &props)
    {
        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;

        options_.AllowTransformationWhileCompacting(2, 4, 16, 1);
        options_.SetTransformType(2);

        options_.IncreaseParallelism(16);
        options_.level0_slowdown_writes_trigger = 9999999;     
        options_.level0_stop_writes_trigger = 99999999;
        options_.max_open_files = -1;
        options_.level0_file_num_compaction_trigger = 4;

        options_.transformer = std::make_shared<rocksdb::Bytecracker>();
        options_.SetTransformType(2);
        options_.use_direct_reads = true;
        options_.use_direct_io_for_flush_and_compaction = true;

        rocksdb::BlockBasedTableOptions table_options;
        table_options.block_cache = nullptr;  // Disable the block cache
        options_.table_factory = std::shared_ptr<rocksdb::TableFactory>(rocksdb::NewBlockBasedTableFactory(table_options));
        
        /*
        uint64_t nums = stoi(props.GetProperty(CoreWorkload::RECORD_COUNT_PROPERTY));
        uint32_t key_len = stoi(props.GetProperty(CoreWorkload::KEY_LENGTH));
        uint32_t value_len = stoi(props.GetProperty(CoreWorkload::FIELD_LENGTH_PROPERTY));
        uint32_t cache_size = nums * (key_len + value_len) / 10;
        if(cache_size < 8 << 20) {
            cache_size = 8 << 20;
        }
        cache_ = rocksdb::NewLRUCache(cache_size);
        */
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
                std::cout << "column family handle: " << column_family_descriptors[i].name << std::endl;
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
                                             std::string translevel)
    {
        options_.SetCompactingLevelWithinColumnFamilyGroup(0);
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(
            dbname, rocksdb::ColumnFamilyOptions(options_)));
      
        std::string prefix = dbname + "_sys_cf";
        std::queue<int> parents;
        parents.push(options_.num_columns);

        for (int level = 1; level < options_.compacting_column_family_num_levels; level++) {
            int queueLen = parents.size();

            for (int j = 0; j < queueLen; j++) {
                int parent_cols = parents.front();
                parents.pop();
                if (parent_cols < 2) {
                    continue;
                }
                rocksdb::Options cfoptions = options_;
                cfoptions.SetCompactingLevelWithinColumnFamilyGroup(level);

                int child1 = parent_cols/2;
                std::string cfname1 = prefix + "_L" + std::to_string(level) + "_G" + std::to_string(j*2);
                column_families.push_back(rocksdb::ColumnFamilyDescriptor(cfname1, rocksdb::ColumnFamilyOptions(cfoptions)));
                parents.push(child1);

                int child2 = parent_cols - child1;
                std::string cfname2 = prefix + "_L" + std::to_string(level) + "_G" + std::to_string(j*2+1);
                column_families.push_back(rocksdb::ColumnFamilyDescriptor(cfname2, rocksdb::ColumnFamilyOptions(cfoptions)));
                parents.push(child2);
            }
        }
    }

}
