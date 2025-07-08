//
//  client.h
//  YCSB-C
//
//

#ifndef YCSB_C_CLIENT_H_
#define YCSB_C_CLIENT_H_

#include <string>
#include <atomic>
#include "db.h"
#include "core_workload.h"
#include "utils.h"
#include "data.pb.h"

using namespace std;

extern atomic<uint64_t> ops_cnt[ycsbc::Operation::READMODIFYWRITE + 1] ;    //操作个数
extern atomic<uint64_t> ops_time[ycsbc::Operation::READMODIFYWRITE + 1] ;   //微秒

namespace ycsbc {

class Client {
 public:
  Client(DB &db, CoreWorkload &wl) : db_(db), workload_(wl) { }
  
  virtual bool DoInsert();
  virtual bool DoTransaction();
  virtual bool DoRead();
  
  virtual ~Client() { }
  
 protected:
  
  virtual int TransactionRead();
  virtual int TransactionReadModifyWrite();
  virtual int TransactionScan();
  virtual int TransactionUpdate();
  virtual int TransactionInsert();
  
  DB &db_;
  CoreWorkload &workload_;
};

inline bool Client::DoInsert() {
  std::string key = workload_.NextSequenceKey();
  std::string val;
  if (workload_.input_data_format() == "json") {
    std::string val = workload_.BuildJsonRecord(workload_.column_data_type());
    return (db_.Insert(workload_.NextTable(), key, val) == DB::kOK);
  } else {
    data::Row value;
    workload_.BuildProtoRecord(value, workload_.column_data_type());
    std::string serializedValue;
    value.SerializeToString(&serializedValue);
    return (db_.Insert(workload_.NextTable(), key, serializedValue) == DB::kOK);
  } 
}

inline bool Client::DoRead() {
  return (TransactionRead() == DB::kOK);
}

// major benchmark. Will implement the 5 queries in it. 
// Query 1: Insert into T values (C1, C2, C3, ...., Cn)
inline bool Client::DoTransaction() {
  int status = -1;
  uint64_t start_time = get_now_micros();

  switch (workload_.NextOperation()) {
    case INSERT:
      status = TransactionInsert();
      ops_time[INSERT].fetch_add((get_now_micros() - start_time ), std::memory_order_relaxed);
      ops_cnt[INSERT].fetch_add(1, std::memory_order_relaxed);
      break;
    case READ:
      status = TransactionRead();
      ops_time[READ].fetch_add((get_now_micros() - start_time ), std::memory_order_relaxed);
      ops_cnt[READ].fetch_add(1, std::memory_order_relaxed);
      break;
    case UPDATE:
      status = TransactionUpdate();
      ops_time[UPDATE].fetch_add((get_now_micros() - start_time ), std::memory_order_relaxed);
      ops_cnt[UPDATE].fetch_add(1, std::memory_order_relaxed);
      break;
    case SCAN:
      status = TransactionScan();
      ops_time[SCAN].fetch_add((get_now_micros() - start_time ), std::memory_order_relaxed);
      ops_cnt[SCAN].fetch_add(1, std::memory_order_relaxed);
      break;
    case READMODIFYWRITE:
      status = TransactionReadModifyWrite();
      ops_time[READMODIFYWRITE].fetch_add((get_now_micros() - start_time ), std::memory_order_relaxed);
      ops_cnt[READMODIFYWRITE].fetch_add(1, std::memory_order_relaxed);
      break;
    default:
      throw utils::Exception("Operation request is not recognized!");
  }
  assert(status >= 0);
  return (status == DB::kOK);
}

inline int Client::TransactionRead() {
  const std::string &table = workload_.NextTable();
  const std::string &key = workload_.index_access()
                           ? workload_.NextIndexKey()
                           : workload_.NextTransactionKey();
  //std::vector<DB::KVPair> result;
  std::string result;
  std::string req_dist = workload_.request_distribution();
  if (!workload_.read_all_fields()) {
    std::set<int> fields;
    //fields.push_back("field" + workload_.NextFieldName());
    fields.insert(0);
    return db_.Read(table, key, &fields, req_dist, workload_.index_access(), result);
  } else {
    return db_.Read(table, key, NULL, req_dist, workload_.index_access(), result);
  }
}

inline int Client::TransactionReadModifyWrite() {
  const std::string &table = workload_.NextTable();
  const std::string &key = workload_.NextTransactionKey();
  //std::vector<DB::KVPair> result;
  std::string result;
  if (!workload_.read_all_fields()) {
    std::set<int> fields;
    //fields.push_back("field" + workload_.NextFieldName());
    fields.insert(0);
    db_.Read(table, key, &fields, workload_.request_distribution(), workload_.index_access(), result);
  } else {
    db_.Read(table, key, NULL, workload_.request_distribution(), workload_.index_access(), result);
  }
  
  if (workload_.input_data_format() == "json") {
    std::string val;
    if (workload_.write_all_fields()) {
      val = workload_.BuildJsonRecord(workload_.column_data_type());
    } else {
      val = workload_.BuildJsonColumn(workload_.column_data_type());
    }
    return db_.Update(table, key, val);
  } else {
    data::Row columns;
    if (workload_.write_all_fields()) {
      workload_.BuildProtoRecord(columns, workload_.column_data_type());
    } else {
      workload_.BuildProtoColumn(columns, workload_.column_data_type());
    }
    std::string serializedColumns;
    columns.SerializeToString(&serializedColumns);
    return db_.Update(table, key, serializedColumns);
  }
}

inline int Client::TransactionScan() {
  const std::string &table = workload_.NextTable();

  std::string key;
  std::string end_key;
  if (workload_.index_access()) {
    workload_.NextIndexScanKey(key, end_key);
  } else {
    workload_.NextTransactionScanKey(key, end_key);
  }
  
  //int len = workload_.NextScanLength();
  std::vector<std::string> result;
  if (!workload_.read_all_fields()) {
    std::set<int> fields;
    //fields.push_back("field" + workload_.NextFieldName());
    fields.insert(0);
    return db_.Scan(table, key, end_key, &fields, workload_.request_distribution(), workload_.index_access(), result);
  } else {
    return db_.Scan(table, key, end_key, NULL, workload_.request_distribution(), workload_.index_access(), result);
  }
}

inline int Client::TransactionUpdate() {
  const std::string &table = workload_.NextTable();
  const std::string &key = workload_.NextTransactionKey();

  if (workload_.input_data_format() == "json") {
    std::string val;
    if (workload_.write_all_fields()) {
      val = workload_.BuildJsonRecord(workload_.column_data_type());
    } else {
      val = workload_.BuildJsonColumn(workload_.column_data_type());
    }
    return db_.Update(table, key, val);
  } else {
    data::Row columns;
    if (workload_.write_all_fields()) {
      workload_.BuildProtoRecord(columns, workload_.column_data_type());
    } else {
      workload_.BuildProtoColumn(columns, workload_.column_data_type());
    }
    std::string serializedColumns;
    columns.SerializeToString(&serializedColumns);
    return db_.Update(table, key, serializedColumns);
  }
}

inline int Client::TransactionInsert() {
  const std::string &table = workload_.NextTable();
  const std::string &key = workload_.NextSequenceKey();
  if (workload_.input_data_format() == "json") {
    std::string val = workload_.BuildJsonRecord(workload_.column_data_type());
    return db_.Insert(table, key, val);
  } else {
    data::Row columns;
    workload_.BuildProtoRecord(columns, workload_.column_data_type());
    std::string serializedColumns;
    columns.SerializeToString(&serializedColumns);
    return db_.Insert(table, key, serializedColumns);
  }
} 

} // ycsbc

#endif // YCSB_C_CLIENT_H_
