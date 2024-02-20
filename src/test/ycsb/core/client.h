//
//  client.h
//  YCSB-C
//
//  Created by Jinglei Ren on 12/10/14.
//  Copyright (c) 2014 Jinglei Ren <jinglei@ren.systems>.
//

#ifndef YCSB_C_CLIENT_H_
#define YCSB_C_CLIENT_H_

#include <string>
#include <atomic>
#include "db.h"
#include "core_workload.h"
#include "utils.h"
#include "proto/columns.pb.h"

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

  data::Row value;
  workload_.BuildRecord(value);
  std::string serializedValue;
  value.SerializeToString(&serializedValue);
  return (db_.Insert(workload_.NextTable(), key, serializedValue) == DB::kOK);
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
  const std::string &key = workload_.NextTransactionKey();
  //std::vector<DB::KVPair> result;
  std::string result;
  if (!workload_.read_all_fields()) {
    std::vector<std::string> fields;
    //fields.push_back("field" + workload_.NextFieldName());
    fields.push_back("field1");
    return db_.Read(table, key, &fields, result);
  } else {
    return db_.Read(table, key, NULL, result);
  }
}

inline int Client::TransactionReadModifyWrite() {
  const std::string &table = workload_.NextTable();
  const std::string &key = workload_.NextTransactionKey();
  //std::vector<DB::KVPair> result;
  std::string result;

  if (!workload_.read_all_fields()) {
    std::vector<std::string> fields;
    //fields.push_back("field" + workload_.NextFieldName());
    fields.push_back("field1");
    db_.Read(table, key, &fields, result);
  } else {
    db_.Read(table, key, NULL, result);
  }

  data::Row columns;
  if (workload_.write_all_fields()) {
    workload_.BuildRecord(columns);
  } else {
    workload_.BuildColumn(columns);
  }
  std::string serializedColumns;
  columns.SerializeToString(&serializedColumns);

  return db_.Update(table, key, serializedColumns);
}

inline int Client::TransactionScan() {
  const std::string &table = workload_.NextTable();
  //const std::string &key = workload_.NextTransactionKey();
  //const std::string &max_key = workload_.BuildMaxKey();
  std::string key;
  std::string max_key;
  workload_.NextTransactionScanKey(key, max_key);
  int len = workload_.NextScanLength();
  std::vector<std::string> result;
  if (!workload_.read_all_fields()) {
    std::vector<std::string> fields;
    //fields.push_back("field" + workload_.NextFieldName());
    fields.push_back("field1");
    return db_.Scan(table, key, len, &fields, result);
  } else {
    return db_.Scan(table, key, len, NULL, result);
  }
}

inline int Client::TransactionUpdate() {
  const std::string &table = workload_.NextTable();
  const std::string &key = workload_.NextTransactionKey();

  data::Row columns;
  if (workload_.write_all_fields()) {
    workload_.BuildRecord(columns);
  } else {
    workload_.BuildColumn(columns);
  }
  std::string serializedColumns;
  columns.SerializeToString(&serializedColumns);
  return db_.Update(table, key, serializedColumns);
}

inline int Client::TransactionInsert() {
  const std::string &table = workload_.NextTable();
  const std::string &key = workload_.NextSequenceKey();

  data::Row columns;
  workload_.BuildRecord(columns);
  std::string serializedColumns;
  columns.SerializeToString(&serializedColumns);
  return db_.Insert(table, key, serializedColumns);
} 

} // ycsbc

#endif // YCSB_C_CLIENT_H_
