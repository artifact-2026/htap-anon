
#ifndef YCSB_C_CORE_WORKLOAD_H_
#define YCSB_C_CORE_WORKLOAD_H_

#include <vector>
#include <string>
#include <cstring>
#include <cmath>
#include "db.h"
#include "properties.h"
#include "generator.h"
#include "discrete_generator.h"
#include "counter_generator.h"
#include "utils.h"
#include "data.pb.h"

namespace ycsbc {

enum Operation {
  INSERT,
  READ,
  UPDATE,
  SCAN,
  READMODIFYWRITE
};

class CoreWorkload {
 public:
  /// 
  /// The name of the ceph pool
  ///
  static const std::string POOLNAME_PROPERTY;
  static const std::string POOLNAME_DEFAULT;

  /// 
  /// The name of the database table to run queries against.
  ///
  static const std::string TABLENAME_PROPERTY;
  static const std::string TABLENAME_DEFAULT;

  static const std::string KEY_LENGTH;
  static const std::string KEY_LENGTH_DEFAULT;
  
  /// 
  /// The name of the property for the number of fields in a record.
  ///
  static const std::string FIELD_COUNT_PROPERTY;
  static const std::string FIELD_COUNT_DEFAULT;
  
  /// 
  /// The name of the property for the field length distribution.
  /// Options are "uniform", "zipfian" (favoring short records), and "constant".
  ///
  static const std::string FIELD_LENGTH_DISTRIBUTION_PROPERTY;
  static const std::string FIELD_LENGTH_DISTRIBUTION_DEFAULT;
  
  /// 
  /// The name of the property for the length of a field in bytes.
  ///
  static const std::string FIELD_LENGTH_PROPERTY;
  static const std::string FIELD_LENGTH_DEFAULT;

  ///
  /// Whether this is an index read or scan
  ///
  static const std::string INDEX_ACCESS_PROPERTY;
  static const std::string INDEX_ACCESS_DEFAULT;
  
  /// 
  /// The name of the property for deciding whether to read one field (false)
  /// or all fields (true) of a record.
  ///
  static const std::string READ_ALL_FIELDS_PROPERTY;
  static const std::string READ_ALL_FIELDS_DEFAULT;

  /// 
  /// The name of the property for deciding whether to write one field (false)
  /// or all fields (true) of a record.
  ///
  static const std::string WRITE_ALL_FIELDS_PROPERTY;
  static const std::string WRITE_ALL_FIELDS_DEFAULT;
  
  /// 
  /// The name of the property for the proportion of read transactions.
  ///
  static const std::string READ_PROPORTION_PROPERTY;
  static const std::string READ_PROPORTION_DEFAULT;
  
  /// 
  /// The name of the property for the proportion of update transactions.
  ///
  static const std::string UPDATE_PROPORTION_PROPERTY;
  static const std::string UPDATE_PROPORTION_DEFAULT;
  
  /// 
  /// The name of the property for the proportion of insert transactions.
  ///
  static const std::string INSERT_PROPORTION_PROPERTY;
  static const std::string INSERT_PROPORTION_DEFAULT;
  
  /// 
  /// The name of the property for the proportion of scan transactions.
  ///
  static const std::string SCAN_PROPORTION_PROPERTY;
  static const std::string SCAN_PROPORTION_DEFAULT;

  ///
  /// data type
  ///
  static const std::string COLUMN_DATA_TYPE_PROPERTY;
  static const std::string COLUMN_DATA_TYPE_DEFAULT;

  ///
  /// data format
  ///
  static const std::string INPUT_DATA_FORMAT_PROPERTY;
  static const std::string INPUT_DATA_FORMAT_DEFAULT;

  ///
  /// The name of the property for the proportion of
  /// read-modify-write transactions.
  ///
  static const std::string READMODIFYWRITE_PROPORTION_PROPERTY;
  static const std::string READMODIFYWRITE_PROPORTION_DEFAULT;
  
  /// 
  /// The name of the property for the the distribution of request keys.
  /// Options are "uniform", "zipfian" and "latest".
  ///
  static const std::string REQUEST_DISTRIBUTION_PROPERTY;
  static const std::string REQUEST_DISTRIBUTION_DEFAULT;
  
  /// 
  /// The name of the property for the max scan length (number of records).
  ///
  static const std::string MAX_SCAN_LENGTH_PROPERTY;
  static const std::string MAX_SCAN_LENGTH_DEFAULT;
  
  /// 
  /// The name of the property for the scan length distribution.
  /// Options are "uniform" and "zipfian" (favoring short scans).
  ///
  static const std::string SCAN_LENGTH_DISTRIBUTION_PROPERTY;
  static const std::string SCAN_LENGTH_DISTRIBUTION_DEFAULT;

  /// 
  /// The name of the property for the order to insert records.
  /// Options are "ordered" or "hashed".
  ///
  static const std::string INSERT_ORDER_PROPERTY;
  static const std::string INSERT_ORDER_DEFAULT;

  static const std::string INSERT_START_PROPERTY;
  static const std::string INSERT_START_DEFAULT;
  
  static const std::string RECORD_COUNT_PROPERTY;
  static const std::string OPERATION_COUNT_PROPERTY;

  ///
  /// Initialize the scenario.
  /// Called once, in the main client thread, before any operations are started.
  ///
  virtual void Init(const utils::Properties &p);
  
  virtual void BuildProtoRecord(data::Row &value);
  virtual void BuildProtoColumn(data::Row &update, std::string name);
  virtual std::string BuildJsonRecord(std::string type);
  virtual std::string BuildJsonColumn(std::string type);
  virtual std::string BuildMaxKey();
  virtual size_t GetRecordLength();

  virtual std::string GetPool() { return pool_name_; }
  virtual std::string NextTable() { return table_name_; }
  virtual std::string NextSequenceKey(); /// Used for loading data
  virtual std::string NextTransactionKey(); /// Used for transactions
  virtual std::string NextIndexKey();   /// used for index read/scan
  virtual void NextTransactionScanKey(std::string &start_key, std::string &end_key);
  virtual void NextIndexScanKey(std::string &start_key, std::string &end_key);
  virtual Operation NextOperation() { return op_chooser_.Next(); }
  virtual std::string NextFieldName();
  virtual size_t NextScanLength() { return scan_len_chooser_->Next(); }
  
  bool read_all_fields() const { return read_all_fields_; }
  bool write_all_fields() const { return write_all_fields_; }
  bool index_access() const { return index_access_; }
  std::string request_distribution() const { return request_distribution_; }
  std::string column_data_type() const { return column_data_type_; }
  std::string input_data_format() const { return input_data_format_; }

  CoreWorkload() :
      key_length_(16), field_count_(0), read_all_fields_(false), write_all_fields_(false), index_access_(false),
      column_data_type_(""), input_data_format_(""), request_distribution_(""), field_len_generator_(NULL),
      key_generator_(NULL), key_chooser_(NULL), field_chooser_(NULL), scan_len_chooser_(NULL),
      insert_key_sequence_(3), ordered_inserts_(true), record_count_(0), max_scan_len_(0)
    {}
  
  virtual ~CoreWorkload() {
    if (field_len_generator_) delete field_len_generator_;
    if (key_generator_) delete key_generator_;
    if (key_chooser_) delete key_chooser_;
    if (field_chooser_) delete field_chooser_;
    if (scan_len_chooser_) delete scan_len_chooser_;
  }
  
 protected:
  static Generator<uint64_t> *GetFieldLenGenerator(const utils::Properties &p);
  std::string BuildKeyName(uint64_t key_num);

  std::string pool_name_;
  std::string table_name_;
  int key_length_;
  int field_count_;
  bool read_all_fields_;
  bool write_all_fields_;
  bool index_access_;
  std::string column_data_type_;
  std::string input_data_format_;
  std::string request_distribution_;
  Generator<uint64_t> *field_len_generator_;
  Generator<uint64_t> *key_generator_;
  DiscreteGenerator<Operation> op_chooser_;
  Generator<uint64_t> *key_chooser_;
  Generator<uint64_t> *field_chooser_;
  Generator<uint64_t> *scan_len_chooser_;
  CounterGenerator insert_key_sequence_;
  bool ordered_inserts_;
  size_t record_count_;
  int max_scan_len_;
};

inline std::string CoreWorkload::NextSequenceKey() {
  uint64_t key_num = key_generator_->Next() % record_count_;
  return BuildKeyName(key_num);
}

inline std::string CoreWorkload::NextTransactionKey() {
  uint64_t key_num;
  do {
    key_num = key_chooser_->Next();
  } while (key_num > insert_key_sequence_.Last());
  return BuildKeyName(key_num);
}

inline std::string CoreWorkload::NextIndexKey() {
  return std::to_string(utils::RandomPrintInt());
}

inline void CoreWorkload::NextIndexScanKey(std::string &start_key, std::string &end_key) {
  int skey = utils::RandomPrintInt();
  int ekey = skey + 4;
  start_key = std::to_string(skey);
  end_key = std::to_string(ekey);
}

inline void CoreWorkload::NextTransactionScanKey(std::string &start_key, std::string &end_key) {
  uint64_t key_num;
  uint64_t scan_interval_ = 4;
  do {
    key_num = key_chooser_->Next();
  } while (key_num > insert_key_sequence_.Last());
  start_key = BuildKeyName(key_num);
  end_key = BuildKeyName(key_num + scan_interval_);
  //end_key = start_key;
  int index = log10(record_count_ / max_scan_len_);
  index = (index - 2) > 0 ? index - 2 : 0; //increase the end by 1 bit
  end_key[index]++;
}

/* inline std::string CoreWorkload::BuildKeyName(uint64_t key_num) {
  if (!ordered_inserts_) {
    key_num = utils::Hash(key_num);
  }
  return std::string("user").append(std::to_string(key_num));
} */
static inline void fillchar8wirhint64(char *key, uint64_t value) {
        key[0] = ((char)(value >> 56)) & 0xff;
        key[1] = ((char)(value >> 48)) & 0xff;
        key[2] = ((char)(value >> 40) )& 0xff;
        key[3] = ((char)(value >> 32)) & 0xff;
        key[4] = ((char)(value >> 24)) & 0xff;
        key[5] = ((char)(value >> 16)) & 0xff;
        key[6] = ((char)(value >> 8)) & 0xff;
        key[7] = ((char)value) & 0xff;
}

inline std::string CoreWorkload::BuildKeyName(uint64_t key_num) {
  if (!ordered_inserts_) {
    key_num = utils::Hash(key_num);
  }
  char key_buff[key_length_ + 1];
  snprintf(key_buff, key_length_ + 1, "%0*lx", key_length_, key_num);
  //key的前缀以0对齐，长度不超过key_length_，因为snprintf会复制最后字符'\0',所以长度+1；
  return std::string(key_buff, key_length_);
}

inline std::string CoreWorkload::BuildMaxKey() {
  char key_buff[key_length_ + 1];
  memset(key_buff, 0xff, key_length_);
  return std::string(key_buff, key_length_);
}


inline std::string CoreWorkload::NextFieldName() {
  return std::string("field").append(std::to_string(field_chooser_->Next()));
}
  
} // ycsbc

#endif // YCSB_C_CORE_WORKLOAD_H_
