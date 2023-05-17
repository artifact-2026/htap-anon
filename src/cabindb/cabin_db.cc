#include <iostream>
#include <cmath>

#include "cabin_db.h"
#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "rocksdb-rados-env/env_librados.h"

namespace CABINDB_NAMESPACE {
    class CabinDBLogger : public rocksdb::Logger {
      public:
        explicit CabinDBLogger() {};
        ~CabinDBLogger() override {};

        void Logv(const char* format, va_list ap) override {
            Logv(rocksdb::INFO_LEVEL, format, ap);
        }

        void Logv(const rocksdb::InfoLogLevel log_level, const char* format, va_list ap) override {
            //dout(ceph::dout::need_dynamic(v));
            char buf[65536];
            vsnprintf(buf, sizeof(buf), format, ap);
           // *_dout << buf << dendl;
        }

    };

   CabinDB::CabinDB(const char *dbfilename, rocksdb::Options& options, std::vector<std::string> &cfshards) {

        std::string db_name = "cabindb";
        std::string config_path = "/etc/ceph/ceph.conf";

        options.env = new rocksdb::EnvLibrados(db_name, config_path, "cabindb_pool");
        options.IncreaseParallelism();
        options.OptimizeLevelStyleCompaction();
        // create the DB if it's not already present
        options.create_if_missing = true;

        options.compaction_style = rocksdb::kCompactionStyleNone;
        options.level0_slowdown_writes_trigger = 3000;
        options.level0_stop_writes_trigger = 5000;
        options.listeners.emplace_back(new CabinCompactor(options));;

        options.info_log.reset(new CabinDBLogger());

        rocksdb::Status s = rocksdb::DB::Open(options,dbfilename,&db_);
        if(!s.ok()){
            std::cerr<<"Can't open rocksdb "<<dbfilename<<" "<<s.ToString()<<std::endl;
            exit(0);
        }

        for (unsigned i = 0; i < cfshards.size(); i++) {
            rocksdb::ColumnFamilyHandle* cf;
            s = db_->CreateColumnFamily(rocksdb::ColumnFamilyOptions(), cfshards[i], &cf);
            if (!s.ok()) {
                std::cerr<<"Can't create column family handle "<<cfshards[i]<<" "
                         <<s.ToString()<<std::endl;
                exit(0);
            }
            cfhandles_.push_back(cf);
        }
    }

    Status CabinDB::Read(const std::string &table, const std::string &key, std::string &value)
    {
        value.clear();
        rocksdb::Status s = db_->Get(rocksdb::ReadOptions(),key,&value);
        if (s.ok()) {
            return Status::kOK;
        }
        if (s.IsNotFound()) {
            return Status::kNotFound;
        }
        return Status::kError;
    }

    Status CabinDB::Scan(const std::string &table, const std::string &key, int len, std::vector<std::string> &values)
    {
        auto it = db_->NewIterator(rocksdb::ReadOptions());
        values.clear();
        it->Seek(key);
        for (int i = 0; i < len && it->Valid(); i++) {
            values.push_back(it->value().ToString());
        }

        if (values.size() > 0) {
            return Status::kOK;
        }
        
        return Status::kNotFound;
    }

    Status CabinDB::Insert(const std::string &table, const std::string &key, std::string &value)
    {
        rocksdb::WriteOptions write_options = rocksdb::WriteOptions();
        rocksdb::Status s = db_->Put(write_options, key, value);

        if (!s.ok()) {
            std::cerr<<"insert error\n"<<std::endl;
            return Status::kError;
        }
        return Status::kOK;
    }

    Status CabinDB::Delete(const std::string &table, const std::string &key)
    {
        rocksdb::WriteOptions write_options = rocksdb::WriteOptions();
        rocksdb::Status s = db_->Delete(write_options,key);

        if (!s.ok()) {
            std::cerr<<"delete error\n"<<std::endl;
            return Status::kError;
        }
        return Status::kOK;
    }

} // namespace CABINDB_NAMESPACE
