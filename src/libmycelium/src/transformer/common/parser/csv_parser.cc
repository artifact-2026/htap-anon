#include "csv_parser.h"   // → mycelium/csv_parser.h

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace mycelium {

namespace {

static inline void TrimInPlace(std::string* s) {
  auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
  size_t b = 0;
  while (b < s->size() && is_ws(static_cast<unsigned char>((*s)[b]))) ++b;
  size_t e = s->size();
  while (e > b && is_ws(static_cast<unsigned char>((*s)[e - 1]))) --e;
  if (b != 0 || e != s->size()) *s = s->substr(b, e - b);
}

static inline void StripLineEndings(std::string* s) {
  if (!s->empty() && s->back() == '\n') {
    s->pop_back();
    if (!s->empty() && s->back() == '\r') s->pop_back();
  } else if (!s->empty() && s->back() == '\r') {
    s->pop_back();
  }
}

static bool SplitCsvLine(const std::string& line, char delim,
                         bool allow_quotes,
                         std::vector<std::string>* out, std::string* err) {
  out->clear();
  std::string cur;
  cur.reserve(line.size());
  bool in_quotes = false;

  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (allow_quotes && c == '"') {
      if (!in_quotes) {
        in_quotes = true;
      } else if (i + 1 < line.size() && line[i + 1] == '"') {
        cur.push_back('"');
        ++i;
      } else {
        in_quotes = false;
      }
      continue;
    }
    if (!in_quotes && c == delim) {
      out->push_back(cur);
      cur.clear();
      continue;
    }
    cur.push_back(c);
  }
  if (in_quotes) { if (err) *err = "Unterminated quoted field"; return false; }
  out->push_back(cur);
  return true;
}

// Convert a FieldSchema type string to a FieldValue.
static FieldValue ParseFieldValue(const std::string& raw,
                                  const std::string& type_str,
                                  bool empty_is_null) {
  std::string s = raw;
  TrimInPlace(&s);

  if (empty_is_null && s.empty()) return FieldValue::MakeNull();

  if (type_str == "int64" || type_str == "i64") {
    try {
      size_t pos = 0;
      long long v = std::stoll(s, &pos, 10);
      if (pos == s.size()) return FieldValue::MakeInt64(static_cast<int64_t>(v));
    } catch (...) {}
    return FieldValue::MakeNull();
  }
  if (type_str == "int32" || type_str == "i32") {
    try {
      size_t pos = 0;
      long long v = std::stoll(s, &pos, 10);
      if (pos == s.size() &&
          v >= std::numeric_limits<int32_t>::min() &&
          v <= std::numeric_limits<int32_t>::max())
        return FieldValue::MakeInt32(static_cast<int32_t>(v));
    } catch (...) {}
    return FieldValue::MakeNull();
  }
  if (type_str == "uint64" || type_str == "u64") {
    try {
      size_t pos = 0;
      unsigned long long v = std::stoull(s, &pos, 10);
      if (pos == s.size()) return FieldValue::MakeUint64(static_cast<uint64_t>(v));
    } catch (...) {}
    return FieldValue::MakeNull();
  }
  if (type_str == "uint32" || type_str == "u32") {
    try {
      size_t pos = 0;
      unsigned long long v = std::stoull(s, &pos, 10);
      if (pos == s.size() && v <= std::numeric_limits<uint32_t>::max())
        return FieldValue::MakeUint32(static_cast<uint32_t>(v));
    } catch (...) {}
    return FieldValue::MakeNull();
  }
  if (type_str == "double" || type_str == "f64") {
    try {
      size_t pos = 0;
      double v = std::stod(s, &pos);
      if (pos == s.size()) return FieldValue::MakeDouble(v);
    } catch (...) {}
    return FieldValue::MakeNull();
  }
  if (type_str == "float" || type_str == "f32") {
    try {
      size_t pos = 0;
      float v = std::stof(s, &pos);
      if (pos == s.size()) return FieldValue::MakeFloat(v);
    } catch (...) {}
    return FieldValue::MakeNull();
  }
  if (type_str == "bool" || type_str == "boolean") {
    std::string l = s;
    std::transform(l.begin(), l.end(), l.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    if (l == "true" || l == "t" || l == "1")  return FieldValue::MakeBool(true);
    if (l == "false"|| l == "f" || l == "0")  return FieldValue::MakeBool(false);
    return FieldValue::MakeNull();
  }
  if (type_str == "binary") {
    return FieldValue::MakeBytes(s);
  }
  // Default: treat as string (most robust for CSV).
  return FieldValue::MakeBytes(s);
}

}  // namespace

CsvParser::CsvParser(std::vector<FieldSchema> input_schema, CsvParserOptions opts)
    : input_schema_(std::move(input_schema)), opts_(opts) {}

bool CsvParser::Validate(const ByteBuffer& input_data) const {
  if (input_data.empty()) return false;
  std::string line(reinterpret_cast<const char*>(input_data.data()), input_data.size());
  StripLineEndings(&line);
  std::vector<std::string> fields;
  std::string err;
  if (!SplitCsvLine(line, opts_.delimiter, opts_.allow_quoted_fields, &fields, &err))
    return false;
  if (!input_schema_.empty() && fields.size() != input_schema_.size()) return false;
  return true;
}

Result<ParsedRow> CsvParser::Parse(const ByteBuffer& data) const {
  std::string line(reinterpret_cast<const char*>(data.data()), data.size());
  StripLineEndings(&line);

  std::vector<std::string> fields;
  std::string err;
  if (!SplitCsvLine(line, opts_.delimiter, opts_.allow_quoted_fields, &fields, &err))
    return Result<ParsedRow>::Err("CsvParser::Parse: " + err);

  // Resolve schema: either configured or inferred as string columns.
  std::vector<FieldSchema> schema = input_schema_;
  if (schema.empty()) {
    schema.reserve(fields.size());
    for (size_t i = 0; i < fields.size(); ++i) {
      FieldSchema fs;
      fs.name         = "col" + std::to_string(i);
      fs.type         = "string";
      fs.field_number = static_cast<int>(i);
      schema.push_back(std::move(fs));
    }
  } else if (fields.size() != schema.size()) {
    return Result<ParsedRow>::Err(
        "CsvParser::Parse: column count mismatch: got " +
        std::to_string(fields.size()) + " expected " + std::to_string(schema.size()));
  }

  ParsedRow row;
  row.fields.reserve(schema.size());

  for (size_t i = 0; i < schema.size(); ++i) {
    ParsedField pf;
    pf.name         = schema[i].name;
    pf.field_number = schema[i].field_number;
    pf.value        = ParseFieldValue(fields[i], schema[i].type, opts_.empty_is_null);
    row.fields.push_back(std::move(pf));
  }

  return Result<ParsedRow>::Ok(std::move(row));
}

}  // namespace mycelium
