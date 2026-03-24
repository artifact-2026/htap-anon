#include "json_parser.h"   // → mycelium/json_parser.h

#include <cstring>
#include <string>
#include <vector>

namespace mycelium {

JsonColsParser::JsonColsParser(size_t num_cols, size_t expected_value_len)
    : num_cols_(num_cols), expected_value_len_(expected_value_len) {
  input_schema_.reserve(num_cols_);
  for (size_t i = 0; i < num_cols_; ++i) {
    FieldSchema fs;
    fs.name         = "col" + std::to_string(i);
    fs.type         = "bytes";
    fs.field_number = static_cast<int>(i + 1);
    input_schema_.push_back(std::move(fs));
  }
}

bool JsonColsParser::Validate(const ByteBuffer& input_data) const {
  if (input_data.empty()) return false;
  const char* s = reinterpret_cast<const char*>(input_data.data());
  const size_t n = input_data.size();
  return n >= 2 && s[0] == '{' && s[n - 1] == '}';
}

// static
bool JsonColsParser::ExtractStringValueForKey(const char* json,
                                              size_t n,
                                              const std::string& key,
                                              const char** out_begin,
                                              const char** out_end) {
  const std::string needle = "\"" + key + "\"";
  const char* end = json + n;
  const char* p = nullptr;

  for (const char* cur = json; cur + needle.size() <= end; ++cur) {
    if (std::memcmp(cur, needle.data(), needle.size()) == 0) {
      p = cur + needle.size();
      break;
    }
  }
  if (!p) return false;

  while (p < end && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) ++p;
  if (p >= end || *p != ':') return false;
  ++p;
  while (p < end && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) ++p;
  if (p >= end || *p != '"') return false;
  ++p;

  const char* val_begin = p;
  while (p < end && *p != '"') ++p;
  if (p >= end) return false;

  *out_begin = val_begin;
  *out_end   = p;
  return true;
}

Result<ParsedRow> JsonColsParser::Parse(const ByteBuffer& data) const {
  if (!Validate(data))
    return Result<ParsedRow>::Err("JsonColsParser::Parse: invalid JSON envelope");

  const char* s = reinterpret_cast<const char*>(data.data());
  const size_t n = data.size();

  ParsedRow row;
  row.fields.reserve(num_cols_);

  for (size_t i = 0; i < num_cols_; ++i) {
    const std::string key = "col" + std::to_string(i);
    const char* vb = nullptr;
    const char* ve = nullptr;

    if (!ExtractStringValueForKey(s, n, key, &vb, &ve))
      return Result<ParsedRow>::Err("JsonColsParser::Parse: missing key: " + key);

    const size_t len = static_cast<size_t>(ve - vb);
    if (expected_value_len_ != 0 && len != expected_value_len_) {
      return Result<ParsedRow>::Err(
          "JsonColsParser::Parse: value length mismatch for key: " + key);
    }

    ParsedField pf;
    pf.name         = key;
    pf.field_number = static_cast<int>(i + 1);
    pf.value        = FieldValue::MakeBytes(std::string(vb, ve));
    row.fields.push_back(std::move(pf));
  }

  return Result<ParsedRow>::Ok(std::move(row));
}

}  // namespace mycelium
