//
//  core_workload.cc
//  YCSB-C
//
//

#include <nlohmann/json.hpp>
#include <string>

#include "uniform_generator.h"
#include "zipfian_generator.h"
#include "scrambled_zipfian_generator.h"
#include "skewed_latest_generator.h"
#include "skewed_leastrecent_generator.h"
#include "const_generator.h"
#include "core_workload.h"

using ycsbc::CoreWorkload;
using std::string;

const string CoreWorkload::POOLNAME_PROPERTY = "poolname";
const string CoreWorkload::POOLNAME_DEFAULT = "cephlsm";

const string CoreWorkload::TABLENAME_PROPERTY = "table";
const string CoreWorkload::TABLENAME_DEFAULT = "mycelium";

const string CoreWorkload::KEY_LENGTH = "keylength";
const string CoreWorkload::KEY_LENGTH_DEFAULT = "16";

const string CoreWorkload::FIELD_COUNT_PROPERTY = "fieldcount";
const string CoreWorkload::FIELD_COUNT_DEFAULT = "10";

const string CoreWorkload::FIELD_LENGTH_DISTRIBUTION_PROPERTY =
    "field_len_dist";
const string CoreWorkload::FIELD_LENGTH_DISTRIBUTION_DEFAULT = "constant";

const string CoreWorkload::FIELD_LENGTH_PROPERTY = "fieldlength";
const string CoreWorkload::FIELD_LENGTH_DEFAULT = "100";

const string CoreWorkload::INDEX_ACCESS_PROPERTY = "indexaccess";
const string CoreWorkload::INDEX_ACCESS_DEFAULT = "false";

const string CoreWorkload::READ_ALL_FIELDS_PROPERTY = "readallfields";
const string CoreWorkload::READ_ALL_FIELDS_DEFAULT = "true";

const string CoreWorkload::WRITE_ALL_FIELDS_PROPERTY = "writeallfields";
const string CoreWorkload::WRITE_ALL_FIELDS_DEFAULT = "false";

const string CoreWorkload::READ_PROPORTION_PROPERTY = "readproportion";
const string CoreWorkload::READ_PROPORTION_DEFAULT = "0.95";

const string CoreWorkload::UPDATE_PROPORTION_PROPERTY = "updateproportion";
const string CoreWorkload::UPDATE_PROPORTION_DEFAULT = "0.05";

const string CoreWorkload::INSERT_PROPORTION_PROPERTY = "insertproportion";
const string CoreWorkload::INSERT_PROPORTION_DEFAULT = "0.0";

const string CoreWorkload::SCAN_PROPORTION_PROPERTY = "scanproportion";
const string CoreWorkload::SCAN_PROPORTION_DEFAULT = "0.0";

const string CoreWorkload::READMODIFYWRITE_PROPORTION_PROPERTY =
    "readmodifywriteproportion";
const string CoreWorkload::READMODIFYWRITE_PROPORTION_DEFAULT = "0.0";

const string CoreWorkload::REQUEST_DISTRIBUTION_PROPERTY =
    "requestdistribution";
const string CoreWorkload::REQUEST_DISTRIBUTION_DEFAULT = "uniform";

const string CoreWorkload::COLUMN_DATA_TYPE_PROPERTY = "columndatatype";
const string CoreWorkload::COLUMN_DATA_TYPE_DEFAULT = "mixed";

const string CoreWorkload::INPUT_DATA_FORMAT_PROPERTY = "inputdataformat";
const string CoreWorkload::INPUT_DATA_FORMAT_DEFAULT = "protobuf";

const string CoreWorkload::MAX_SCAN_LENGTH_PROPERTY = "maxscanlength";
const string CoreWorkload::MAX_SCAN_LENGTH_DEFAULT = "1000";

const string CoreWorkload::SCAN_LENGTH_DISTRIBUTION_PROPERTY =
    "scanlengthdistribution";
const string CoreWorkload::SCAN_LENGTH_DISTRIBUTION_DEFAULT = "uniform";

const string CoreWorkload::INSERT_ORDER_PROPERTY = "insertorder";
const string CoreWorkload::INSERT_ORDER_DEFAULT = "hashed";

const string CoreWorkload::INSERT_START_PROPERTY = "insertstart";
const string CoreWorkload::INSERT_START_DEFAULT = "0";

const string CoreWorkload::RECORD_COUNT_PROPERTY = "recordcount";
const string CoreWorkload::TOTAL_RECORD_COUNT_PROPERTY = "totalrecordcount";
const string CoreWorkload::OPERATION_COUNT_PROPERTY = "operationcount";

void CoreWorkload::Init(const utils::Properties &p) {
  pool_name_ = p.GetProperty(POOLNAME_PROPERTY,POOLNAME_DEFAULT);
  table_name_ = p.GetProperty(TABLENAME_PROPERTY,TABLENAME_DEFAULT);
  
  key_length_ = std::stoi(p.GetProperty(KEY_LENGTH,
                                         KEY_LENGTH_DEFAULT));

  field_count_ = std::stoi(p.GetProperty(FIELD_COUNT_PROPERTY,
                                         FIELD_COUNT_DEFAULT));
  field_len_generator_ = GetFieldLenGenerator(p);
  
  double read_proportion = std::stod(p.GetProperty(READ_PROPORTION_PROPERTY,
                                                   READ_PROPORTION_DEFAULT));
  double update_proportion = std::stod(p.GetProperty(UPDATE_PROPORTION_PROPERTY,
                                                     UPDATE_PROPORTION_DEFAULT));
  double insert_proportion = std::stod(p.GetProperty(INSERT_PROPORTION_PROPERTY,
                                                     INSERT_PROPORTION_DEFAULT));
  double scan_proportion = std::stod(p.GetProperty(SCAN_PROPORTION_PROPERTY,
                                                   SCAN_PROPORTION_DEFAULT));
  double readmodifywrite_proportion = std::stod(p.GetProperty(
      READMODIFYWRITE_PROPORTION_PROPERTY, READMODIFYWRITE_PROPORTION_DEFAULT));
  
  record_count_ = std::stoi(p.GetProperty(RECORD_COUNT_PROPERTY));
  total_record_count_ = std::stoi(p.GetProperty(TOTAL_RECORD_COUNT_PROPERTY));
  request_distribution_ = p.GetProperty(REQUEST_DISTRIBUTION_PROPERTY,
                                           REQUEST_DISTRIBUTION_DEFAULT);
  max_scan_len_ = std::stoi(p.GetProperty(MAX_SCAN_LENGTH_PROPERTY,
                                             MAX_SCAN_LENGTH_DEFAULT));
  
  std::string scan_len_dist = p.GetProperty(SCAN_LENGTH_DISTRIBUTION_PROPERTY,
                                            SCAN_LENGTH_DISTRIBUTION_DEFAULT);
  int insert_start = std::stoi(p.GetProperty(INSERT_START_PROPERTY,
                                             INSERT_START_DEFAULT));
  
  read_all_fields_ = utils::StrToBool(p.GetProperty(READ_ALL_FIELDS_PROPERTY,
                                                    READ_ALL_FIELDS_DEFAULT));
  write_all_fields_ = utils::StrToBool(p.GetProperty(WRITE_ALL_FIELDS_PROPERTY,
                                                     WRITE_ALL_FIELDS_DEFAULT));
  index_access_ = utils::StrToBool(p.GetProperty(INDEX_ACCESS_PROPERTY,
                                                 INDEX_ACCESS_DEFAULT));
  column_data_type_ = p.GetProperty(COLUMN_DATA_TYPE_PROPERTY, COLUMN_DATA_TYPE_DEFAULT);
  input_data_format_ = p.GetProperty(INPUT_DATA_FORMAT_PROPERTY, INPUT_DATA_FORMAT_DEFAULT);
  
  if (p.GetProperty(INSERT_ORDER_PROPERTY, INSERT_ORDER_DEFAULT) == "hashed") {
    ordered_inserts_ = false;
  } else {
    ordered_inserts_ = true;
  }
  
  key_generator_ = new CounterGenerator(insert_start);
  
  if (read_proportion > 0) {
    op_chooser_.AddValue(READ, read_proportion);
  }
  if (update_proportion > 0) {
    op_chooser_.AddValue(UPDATE, update_proportion);
  }
  if (insert_proportion > 0) {
    op_chooser_.AddValue(INSERT, insert_proportion);
  }
  if (scan_proportion > 0) {
    op_chooser_.AddValue(SCAN, scan_proportion);
  }
  if (readmodifywrite_proportion > 0) {
    op_chooser_.AddValue(READMODIFYWRITE, readmodifywrite_proportion);
  }
  
  insert_key_sequence_.Set(record_count_);
  
  if (request_distribution_ == "uniform") {
    key_chooser_ = new UniformGenerator(0, record_count_ - 1);
  } else if (request_distribution_ == "zipfian") {
    // If the number of keys changes, we don't want to change popular keys.
    // So we construct the scrambled zipfian generator with a keyspace
    // that is larger than what exists at the beginning of the test.
    // If the generator picks a key that is not inserted yet, we just ignore it
    // and pick another key.
    int op_count = std::stoi(p.GetProperty(OPERATION_COUNT_PROPERTY));
    int new_keys = (int)(op_count * insert_proportion * 2); // a fudge factor
    key_chooser_ = new ScrambledZipfianGenerator(record_count_ + new_keys);
  } else if (request_distribution_ == "latest") {
    key_chooser_ = new SkewedLatestGenerator(insert_key_sequence_);
  } else if (request_distribution_ == "leastrecent") {
    key_chooser_ = new SkewedLeastRecentGenerator(insert_key_sequence_);
  } else {
    throw utils::Exception("Unknown request distribution: " + request_distribution_);
  }
  
  field_chooser_ = new UniformGenerator(0, field_count_ - 1);
  
  if (scan_len_dist == "uniform") {
    scan_len_chooser_ = new UniformGenerator(1, max_scan_len_);
  } else if (scan_len_dist == "zipfian") {
    scan_len_chooser_ = new ZipfianGenerator(1, max_scan_len_);
  } else {
    throw utils::Exception("Distribution not allowed for scan length: " +
        scan_len_dist);
  }
}

ycsbc::Generator<uint64_t> *CoreWorkload::GetFieldLenGenerator(
    const utils::Properties &p) {
  string field_len_dist = p.GetProperty(FIELD_LENGTH_DISTRIBUTION_PROPERTY,
                                        FIELD_LENGTH_DISTRIBUTION_DEFAULT);
  int field_len = std::stoi(p.GetProperty(FIELD_LENGTH_PROPERTY,
                                          FIELD_LENGTH_DEFAULT));
  if(field_len_dist == "constant") {
    return new ConstGenerator(field_len);
  } else if(field_len_dist == "uniform") {
    return new UniformGenerator(1, field_len);
  } else if(field_len_dist == "zipfian") {
    return new ZipfianGenerator(1, field_len);
  } else {
    throw utils::Exception("Unknown field length distribution: " +
        field_len_dist);
  }
}

void CoreWorkload::BuildProtoRecord(data::ByteRow &value) {
  for (int i = 0; i < field_count_; ++i) {
    auto* c = value.add_values();
    std::string s(field_len_generator_->Next(), utils::RandomPrintChar());
    c->set_value(s);
  }
}

// Format x as a zero-padded 10-digit decimal string, digits only.
inline std::string To10DigitString(uint64_t x) {
  static constexpr uint64_t kMod = 10000000000ULL; // 10^10
  x %= kMod;

  char buf[10];
  // Fill from the end
  for (int i = 9; i >= 0; --i) {
    buf[i] = static_cast<char>('0' + (x % 10));
    x /= 10;
  }
  return std::string(buf, 10);
}

std::string CoreWorkload::BuildJsonRecord(int num_cols) {
  if (num_cols <= 0) return "{}";
  int json_pool_size_ = 100;

  std::call_once(random_ints_once_, [this, num_cols, json_pool_size_] {
    json_cols_built_for_ = num_cols;

    json_values_.clear();
    json_values_.reserve(static_cast<size_t>(json_pool_size_));

    // Prebuild json_pool_size_ distinct records.
    for (int r = 0; r < json_pool_size_; ++r) {
      // Rough reserve to avoid repeated reallocations.
      // Each column contributes roughly: ,"colX":"0123456789"
      std::string out;
      out.reserve(static_cast<size_t>(2 + num_cols * 24));

      out.push_back('{');
      for (int i = 0; i < num_cols; ++i) {
        if (i) out.push_back(',');

        // Key: "col<i>"
        out += "\"col";
        out += std::to_string(i);
        out += "\":\"";

        // Value: 10-digit numeric string
        // Make it unique per record and per column.
        uint64_t v = static_cast<uint64_t>(r) * 1000003ULL + static_cast<uint64_t>(i);
        out += To10DigitString(v);

        out += '"';
      }
      out.push_back('}');
      json_values_.push_back(std::move(out));
    }

    if (json_values_.empty()) {
      json_values_.push_back("{}");
    }
  });

  // Optional safety: fail loudly if called with a different num_cols later.
  if (json_cols_built_for_ != num_cols) {
    throw std::runtime_error("BuildJsonRecord called with different num_cols than initial build");
  }

  uint64_t k = json_next_.fetch_add(1, std::memory_order_relaxed);
  return json_values_[static_cast<size_t>(k % json_values_.size())];
}
/*
void CoreWorkload::prepareJsonValues(int num_ints, std::string type) {
  json_values_.clear();
  json_values_.reserve(static_cast<size_t>(num_ints));

  for (int i = 0; i < num_ints; i++) {
    nlohmann::json jsonData;
    for (int j = 0; j < field_count_; j++) {
      std::string col_name = "field"+std::to_string(j);
      if (type == "numeric") {
        jsonData[col_name] = utils::RandomPrintInt();
      } else if (type == "string") {
        jsonData[col_name] = std::string(field_len_generator_->Next(), utils::RandomPrintChar());
      } else {
        if (j < field_count_/2) {
          jsonData[col_name] = utils::RandomPrintInt();
        } else {
          jsonData[col_name] = std::string(field_len_generator_->Next(), utils::RandomPrintChar());
        }
      }
    }
    std::string jsonString = jsonData.dump();
    json_values_.push_back(jsonString);
  }
}

std::string CoreWorkload::BuildJsonRecord(std::string type) {
  // One-time, thread-safe initialization per CoreWorkload instance
  std::call_once(random_ints_once_, [this, type]{
    prepareJsonValues(10, type);
  });

  return json_values_[rand() % 10];
}*/

void CoreWorkload::BuildProtoColumn(data::ByteRow &value, std::string name) {
  auto* c = value.add_values();
  c->set_value(std::string(field_len_generator_->Next(), utils::RandomPrintChar()));
}

std::string CoreWorkload::BuildJsonColumn(std::string type) {
  nlohmann::json jsonData;
  std::string colname = NextFieldName();

  if (type == "numeric") {
    jsonData[colname] = utils::RandomPrintInt();
  } else {
    jsonData[colname] = std::string(field_len_generator_->Next(), utils::RandomPrintChar());
  }
  
  std::string jsonString = jsonData.dump();
  return jsonString;
}

static inline void AppendFixed64LE(std::string* out, std::uint64_t v) {
  char buf[8];
  // Encode little-endian explicitly
  buf[0] = static_cast<char>( v        & 0xFF);
  buf[1] = static_cast<char>((v >>  8) & 0xFF);
  buf[2] = static_cast<char>((v >> 16) & 0xFF);
  buf[3] = static_cast<char>((v >> 24) & 0xFF);
  buf[4] = static_cast<char>((v >> 32) & 0xFF);
  buf[5] = static_cast<char>((v >> 40) & 0xFF);
  buf[6] = static_cast<char>((v >> 48) & 0xFF);
  buf[7] = static_cast<char>((v >> 56) & 0xFF);
  out->append(buf, 8);
}

std::string CoreWorkload::BuildFixedBinaryRecord(int num_cols) {
  std::string out;
  out.reserve(static_cast<size_t>(num_cols) * 8);

  for (int i = 0; i < num_cols; ++i) {
    std::uint64_t v = static_cast<std::uint64_t>(utils::RandomPrintInt());
    AppendFixed64LE(&out, v);
  }
  return out;
}

size_t CoreWorkload::GetRecordLength() {
  return record_count_;
}
