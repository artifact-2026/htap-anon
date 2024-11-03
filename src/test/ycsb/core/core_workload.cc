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

const string CoreWorkload::INPUT_DATA_TYPE_PROPERTY = "inputdatatype";
const string CoreWorkload::INPUT_DATA_TYPE_DEFAULT = "protobuf";

const string CoreWorkload::OUTPUT_DATA_TYPE_PROPERTY = "outputdatatype";
const string CoreWorkload::OUTPUT_DATA_TYPE_DEFAULT = "flatbuffers";

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
  input_data_type_ = p.GetProperty(INPUT_DATA_TYPE_PROPERTY,
                                  INPUT_DATA_TYPE_DEFAULT);
  output_data_type_ = p.GetProperty(OUTPUT_DATA_TYPE_PROPERTY,
                                    OUTPUT_DATA_TYPE_DEFAULT);
  
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

void CoreWorkload::BuildRecord(data::Row &value) {
  int half = field_count_/2;
  for (int i = 0; i < field_count_; ++i) {
    data::Column* column = value.add_columns();
    column->set_name("field"+std::to_string(i));
    if (i < half) {
      column->set_value(std::to_string(utils::RandomPrintInt()));
    } else {
      column->set_value(std::string(field_len_generator_->Next(), utils::RandomPrintChar()));
    }
  }
}

std::string CoreWorkload::BuildJsonRecord() {
  int half = field_count_/2;
  nlohmann::json jsonData;
  for (int i = 0; i < field_count_; ++i) {
    std::string col_name = "field"+std::to_string(i);
    if (i < half) {
      jsonData[col_name] = utils::RandomPrintInt();
    } else {
      jsonData[col_name] = std::string(field_len_generator_->Next(), utils::RandomPrintChar());
    }
  }
  std::string jsonString = jsonData.dump();
  return jsonString;
}

void CoreWorkload::BuildColumn(data::Row &value) {
  data::Column* column = value.add_columns();
  std::string colname = NextFieldName();
  column->set_name(colname);

  int valueType = std::stoi(colname.substr(5));
  if (valueType < field_count_/2) {
    column->set_value(std::to_string(utils::RandomPrintInt()));
  } else {
    column->set_value(std::string(field_len_generator_->Next(), utils::RandomPrintChar()));
  }
}

std::string CoreWorkload::BuildJsonColumn() {
  nlohmann::json jsonData;
  std::string colname = NextFieldName();
  int valueType = std::stoi(colname.substr(5));

  if (valueType < field_count_/2) {
    jsonData[colname] = std::to_string(utils::RandomPrintInt());
  } else {
    jsonData[colname] = std::string(field_len_generator_->Next(), utils::RandomPrintChar());
  }
  
  std::string jsonString = jsonData.dump();
  return jsonString;
}

size_t CoreWorkload::GetRecordLength() {
  return record_count_;
}
