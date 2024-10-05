#include "core/core_workload.h"
#include "test_preindexing.h"
#include "lib/coding.h"

using namespace std;

namespace ycsbc {
    TestPreindexing::TestPreindexing(const std::string& dbname, const char *dbfilename, utils::Properties &props) {
        noResults = 0;
        bool bootstrap = utils::StrToBool(props.GetProperty("bootstrap","false"));
        int levels = utils::StrToInt(props.GetProperty("levels", "6"));
        int fieldcount = utils::StrToInt(props.GetProperty("fieldcount", "16"));
        SetOptions(dbfilename, levels, fieldcount, bootstrap);

        std::vector<rocksdb::ColumnFamilyDescriptor> column_family_descriptors;
        GetColumnFamilyDescriptors(dbname, column_family_descriptors);
        std::vector<rocksdb::ColumnFamilyHandle*> cf_handles;

        if (bootstrap) {
            rocksdb::Status s = rocksdb::DB::Open(options_, 
                                              dbfilename,
                                              &rocksdb_);
            if (!s.ok()){
                std::cerr<<"Can't open preindexing "<<dbfilename<<" "<<s.ToString()<<std::endl;
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
                std::cerr<<"Can't open preindexing "<<dbfilename<<" "<<s.ToString()<<std::endl;
                exit(0);
            }
        }
        BuildColumnFamilyHandleMap(column_family_descriptors, cf_handles);
    }

    /*
    * Read is for point query over all columns
    */
    int TestPreindexing::Read(const std::string &table, const std::string &key, const std::set<std::string> *fields,
                      const std::string &req_dist, bool index_access, std::string &result) 
    {
        rocksdb::Status s;
        if (index_access) {
            std::string valuekeysstr;
            s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table+"_derived_cf_0"], key, &valuekeysstr);

            std::vector<std::string> valuekeys = deserializeIndex(valuekeysstr);
            for (auto valuekey : valuekeys) {
                s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table], valuekey, &result);
                if (s.ok()) {
                    return 0;
                }
            }
        } else {
            s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table], key, &result);
            if (s.ok()) {
                return 0;
            }
        }
        return 1;
    }

    int TestPreindexing::Scan(const std::string &table, const std::string &begin_key,
                          const std::string &end_key, const std::set<std::string> *fields,
                          const std::string &req_dist, bool index_access,
                          std::vector<std::string> &result) 
    {
        result.clear();
        rocksdb::Status s;

        if (index_access) {
            std::set<std::string> foundkeys;
            auto it = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandles_[table+"_derived_cf_0"]);
            it->Seek(begin_key);
            while (it->Valid()) {
                if (it->key().ToString() < end_key) {
                    std::vector<std::string> rowkeys = deserializeIndex(it->value().ToString());
                    for (auto k : rowkeys) {
                        foundkeys.insert(k);
                    }
                } else {
                    break;
                }
                it->Next();
            }

            for (auto fk : foundkeys) {
                std::string fvalue;
                s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table], fk, &fvalue);
                result.push_back(fvalue);
            }
        } else {
            auto itt = rocksdb_->NewIterator(rocksdb::ReadOptions(), cfhandles_[table]);
            itt->Seek(begin_key);
            while (itt->Valid()) {
                if (itt->key().ToString() < end_key) {
                    result.push_back(itt->value().ToString());
                }
            }
        }

        if (result.size() > 0) {
            return 0;
        }
        return 1;
    }

    int TestPreindexing::Insert(const std::string &table, const std::string &key, std::string &values)
    {
        rocksdb::Status s;

        data::Row row;
        row.ParseFromString(values);
        const std::string ikey = row.columns(1).value();

        // find out if this key was indexed before
        std::string indexed;
        s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table+"_derived_cf_0-helper"], key, &indexed);
        if (indexed != "" && indexed != ikey) {
            // remove the old indexed value
            std::string removekeysstr;
            s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table+"_derived_cf_0"], indexed, &removekeysstr);

            std::vector<std::string> removekeys = deserializeIndex(removekeysstr);
            removekeys.erase(std::remove(removekeys.begin(), removekeys.end(), key), removekeys.end());

            // write back the remaining keys after the removal
            if (removekeys.size() > 0) {
                std::ostringstream oss;

                size_t sz = removekeys.size();
                oss.write(reinterpret_cast<const char*>(&sz), sizeof(sz));

                for (const auto& k : removekeys) {
                    size_t klen = k.size();
                    oss.write(reinterpret_cast<const char*>(&klen), sizeof(klen));
                    oss.write(k.c_str(), klen);
                }
                s = rocksdb_->Put(rocksdb::WriteOptions(), cfhandles_[table+"_derived_cf_0"], indexed, oss.str());
            }
        } else if (indexed == "") {
            // key was never indexed before so we add it
            std::string indvalues;
            s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table+"_derived_cf_0"], ikey, &indvalues);

            std::ostringstream oss;
            size_t key_len = key.size();
            oss.write(reinterpret_cast<const char*>(&key_len), sizeof(key_len));  // Write string length
            oss.write(key.c_str(), key_len);

            indvalues += oss.str();

            s = rocksdb_->Put(rocksdb::WriteOptions(), cfhandles_[table+"_derived_cf_0"], ikey, indvalues);
            if (s.ok()) {
                s = rocksdb_->Put(rocksdb::WriteOptions(), cfhandles_[table], key, values);
                if (s.ok()) {
                    return 0;
                }
            }
        }

        // index was taken care of, now insert the data
        s = rocksdb_->Put(rocksdb::WriteOptions(), cfhandles_[table], key, values);
        if (s.ok()) {
            return 0;
        }

        return 1;
    }

    int TestPreindexing::Update(const std::string &table, const std::string &key, std::string &values)
    {
        return Insert(table, key, values);
    }

    int TestPreindexing::Delete(const std::string &table, const std::string &key)
    {
        std::string values;
        rocksdb::Status s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table], key, &values);

        if (values == "") {
            return 1;
        }

        data::Row row;
        row.ParseFromString(values);
        const std::string ikey = row.columns(1).value();

        std::string origkeystrs;
        s = rocksdb_->Get(rocksdb::ReadOptions(), cfhandles_[table+"_derived_cf_0"], ikey, &origkeystrs);

        std::vector<std::string> origkeys = deserializeIndex(origkeystrs);
        origkeys.erase(std::remove(origkeys.begin(), origkeys.end(), key), origkeys.end());

        if (origkeys.size() > 0) {
            std::ostringstream oss;

            size_t sz = origkeys.size();
            oss.write(reinterpret_cast<const char*>(&sz), sizeof(sz));

            for (const auto& k : origkeys) {
                size_t klen = k.size();
                oss.write(reinterpret_cast<const char*>(&klen), sizeof(klen));
                oss.write(k.c_str(), klen);
            }
            s = rocksdb_->Put(rocksdb::WriteOptions(), cfhandles_[table+"_derived_cf_0"], ikey, oss.str());
        }

        s = rocksdb_->Delete(rocksdb::WriteOptions(), cfhandles_[table], key);

        if (s.ok()) {
            return 0;
        }
        return 1;
    }

    void TestPreindexing::SetOptions(const char *dbfilename, int levels, int fieldcount, bool logging)
    {
        if (!logging) {
            options_.info_log_level = rocksdb::InfoLogLevel::FATAL_LEVEL;
        }
        options_.create_if_missing = true;
        options_.enable_pipelined_write = true;

        options_.num_levels = levels;
        options_.num_columns = fieldcount;
        options_.SetTransformerType(rocksdb::TransformerType::NOTRANSFORMATION);

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
    }

    void TestPreindexing::KeepOnlyRequestedFields(data::Row &row,
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

    void TestPreindexing::GetColumnFamilyDescriptors(const std::string& dbname,
                    std::vector<rocksdb::ColumnFamilyDescriptor>& column_families)
    {
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(dbname,
                                                                  rocksdb::ColumnFamilyOptions(options_)));
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(dbname+"_derived_cf_0",
                                                                  rocksdb::ColumnFamilyOptions(options_)));
        column_families.push_back(rocksdb::ColumnFamilyDescriptor(dbname+"_derived_cf_0-helper",
                                                                  rocksdb::ColumnFamilyOptions(options_)));                   
    }

    void TestPreindexing::BuildColumnFamilyHandleMap(std::vector<rocksdb::ColumnFamilyDescriptor>& column_family_descriptors,
                            std::vector<rocksdb::ColumnFamilyHandle*> handles)
    {
        for (size_t i = 0; i < handles.size(); i++) {
            if (column_family_descriptors[i].name != rocksdb::kDefaultColumnFamilyName) {
                cfhandles_.insert({column_family_descriptors[i].name, handles[i]});
            }
        }
    }

    std::vector<std::string> TestPreindexing::deserializeIndex(const std::string& serialized)
    {
        std::istringstream iss(serialized);
        std::vector<std::string> result;

        size_t num_strings;
        iss.read(reinterpret_cast<char*>(&num_strings), sizeof(num_strings));

        for (size_t i = 0; i < num_strings; i++) {
            size_t str_len;
            iss.read(reinterpret_cast<char*>(&str_len), sizeof(str_len));

            std::string str(str_len, '\0');
            iss.read(&str[0], str_len);

            result.push_back(str);
        }

        return result;
    }

    std::set<int> TestPreindexing::GetQueryingHandles(std::set<std::string> fields) {
        std::set<int> fieldpositions;
        for (auto field : fields) {
            int pos = 0;
            for (size_t i = 5; i < field.size(); i++) {
                pos = pos*10 + field[i] - '0';
            }
            fieldpositions.insert(pos/2);
        }
        return fieldpositions;
    }

}
