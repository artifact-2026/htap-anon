#include "csv_parser.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

#include <arrow/api.h>

namespace mycelium {
namespace {

// Trim only ASCII whitespace on both ends (safe for our CSV tokens).
static inline void TrimInPlace(std::string* s) {
  auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };

  size_t b = 0;
  while (b < s->size() && is_ws(static_cast<unsigned char>((*s)[b]))) ++b;

  size_t e = s->size();
  while (e > b && is_ws(static_cast<unsigned char>((*s)[e - 1]))) --e;

  if (b == 0 && e == s->size()) return;
  *s = s->substr(b, e - b);
} // namespace mycelium

// Remove one trailing '\n' and optional preceding '\r'.
static inline void StripLineEndings(std::string* s) {
  if (!s->empty() && s->back() == '\n') {
    s->pop_back();
    if (!s->empty() && s->back() == '\r') {
      s->pop_back();
    }
  } else if (!s->empty() && s->back() == '\r') {
    s->pop_back();
  }
} // namespace mycelium

// Basic RFC4180-ish split with optional quoted fields and "" escaping.
// Assumes a single record (line).
static bool SplitCsvLine(const std::string& line,
                         char delim,
                         bool allow_quotes,
                         std::vector<std::string>* out_fields,
                         std::string* err) {
  out_fields->clear();

  std::string cur;
  cur.reserve(line.size());

  bool in_quotes = false;

  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];

    if (allow_quotes && c == '"') {
      if (!in_quotes) {
        // Enter quotes only if quote begins a field or after delimiter.
        in_quotes = true;
      } else {
        // In quotes: "" -> literal "
        if (i + 1 < line.size() && line[i + 1] == '"') {
          cur.push_back('"');
          ++i;
        } else {
          in_quotes = false;
        }
      }
      continue;
    }

    if (!in_quotes && c == delim) {
      out_fields->push_back(cur);
      cur.clear();
      continue;
    }

    cur.push_back(c);
  }

  if (in_quotes) {
    if (err) *err = "Unterminated quoted field in CSV record";
    return false;
  }

  out_fields->push_back(cur);
  return true;
} // namespace mycelium

static std::shared_ptr<arrow::DataType> FieldTypeFromString(const std::string& t) {
  // Keep this mapping intentionally small and predictable.
  // You can expand as your schema system grows.
  if (t == "int64" || t == "i64") return arrow::int64();
  if (t == "int32" || t == "i32") return arrow::int32();
  if (t == "uint64" || t == "u64") return arrow::uint64();
  if (t == "uint32" || t == "u32") return arrow::uint32();
  if (t == "double" || t == "f64") return arrow::float64();
  if (t == "float" || t == "f32") return arrow::float32();
  if (t == "bool" || t == "boolean") return arrow::boolean();
  if (t == "binary") return arrow::binary();
  // Default: treat as string (most robust for CSV).
  return arrow::utf8();
} // namespace mycelium

static arrow::Result<std::shared_ptr<arrow::Scalar>> ParseScalar(
    const std::string& raw,
    const std::shared_ptr<arrow::DataType>& dt,
    bool empty_is_null) {
  std::string s = raw;
  TrimInPlace(&s);

  if (empty_is_null && s.empty()) {
    return arrow::MakeNullScalar(dt);
  }

  switch (dt->id()) {
    case arrow::Type::INT64: {
      long long v = 0;
      try {
        size_t pos = 0;
        v = std::stoll(s, &pos, 10);
        if (pos != s.size()) {
          return arrow::Status::Invalid("Invalid int64 token: '", s, "'");
        }
      } catch (...) {
        return arrow::Status::Invalid("Invalid int64 token: '", s, "'");
      }
      return std::make_shared<arrow::Int64Scalar>(static_cast<int64_t>(v));
    }

    case arrow::Type::INT32: {
      long long v = 0;
      try {
        size_t pos = 0;
        v = std::stoll(s, &pos, 10);
        if (pos != s.size()) {
          return arrow::Status::Invalid("Invalid int32 token: '", s, "'");
        }
      } catch (...) {
        return arrow::Status::Invalid("Invalid int32 token: '", s, "'");
      }
      if (v < std::numeric_limits<int32_t>::min() || v > std::numeric_limits<int32_t>::max()) {
        return arrow::Status::Invalid("int32 out of range: '", s, "'");
      }
      return std::make_shared<arrow::Int32Scalar>(static_cast<int32_t>(v));
    }

    case arrow::Type::UINT64: {
      unsigned long long v = 0;
      try {
        size_t pos = 0;
        v = std::stoull(s, &pos, 10);
        if (pos != s.size()) {
          return arrow::Status::Invalid("Invalid uint64 token: '", s, "'");
        }
      } catch (...) {
        return arrow::Status::Invalid("Invalid uint64 token: '", s, "'");
      }
      return std::make_shared<arrow::UInt64Scalar>(static_cast<uint64_t>(v));
    }

    case arrow::Type::UINT32: {
      unsigned long long v = 0;
      try {
        size_t pos = 0;
        v = std::stoull(s, &pos, 10);
        if (pos != s.size()) {
          return arrow::Status::Invalid("Invalid uint32 token: '", s, "'");
        }
      } catch (...) {
        return arrow::Status::Invalid("Invalid uint32 token: '", s, "'");
      }
      if (v > std::numeric_limits<uint32_t>::max()) {
        return arrow::Status::Invalid("uint32 out of range: '", s, "'");
      }
      return std::make_shared<arrow::UInt32Scalar>(static_cast<uint32_t>(v));
    }

    case arrow::Type::DOUBLE: {
      double v = 0.0;
      try {
        size_t pos = 0;
        v = std::stod(s, &pos);
        if (pos != s.size()) {
          return arrow::Status::Invalid("Invalid double token: '", s, "'");
        }
      } catch (...) {
        return arrow::Status::Invalid("Invalid double token: '", s, "'");
      }
      return std::make_shared<arrow::DoubleScalar>(v);
    }

    case arrow::Type::FLOAT: {
      float v = 0.0f;
      try {
        size_t pos = 0;
        v = std::stof(s, &pos);
        if (pos != s.size()) {
          return arrow::Status::Invalid("Invalid float token: '", s, "'");
        }
      } catch (...) {
        return arrow::Status::Invalid("Invalid float token: '", s, "'");
      }
      return std::make_shared<arrow::FloatScalar>(v);
    }

    case arrow::Type::BOOL: {
      // Accept a few common CSV boolean spellings.
      std::string l = s;
      std::transform(l.begin(), l.end(), l.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

      if (l == "true" || l == "t" || l == "1") return std::make_shared<arrow::BooleanScalar>(true);
      if (l == "false" || l == "f" || l == "0") return std::make_shared<arrow::BooleanScalar>(false);

      return arrow::Status::Invalid("Invalid bool token: '", s, "'");
    }

    case arrow::Type::BINARY: {
      // Treat as raw bytes of the string token (no base64 decoding here).
      return std::make_shared<arrow::BinaryScalar>(arrow::Buffer::FromString(s));
    }

    case arrow::Type::STRING: {
      return std::make_shared<arrow::StringScalar>(s);
    }

    default: {
      // Conservative default: store as string.
      return std::make_shared<arrow::StringScalar>(s);
    }
  }
} // namespace mycelium

}  // namespace

CsvParser::CsvParser(std::vector<FieldSchema> input_schema, CsvParserOptions opts)
    : input_schema_(std::move(input_schema)), opts_(opts) {}

bool CsvParser::Validate(const ByteBuffer& input_data) const {
  // Keep Validate cheap: basic non-empty, and if schema is known, column count match.
  if (input_data.empty()) return false;

  std::string line(reinterpret_cast<const char*>(input_data.data()), input_data.size());
  StripLineEndings(&line);

  std::vector<std::string> fields;
  std::string err;
  if (!SplitCsvLine(line, opts_.delimiter, opts_.allow_quoted_fields, &fields, &err)) {
    return false;
  }

  if (!input_schema_.empty() && fields.size() != input_schema_.size()) {
    return false;
  }
  return true;
} // namespace mycelium

arrow::Result<ArrowRecord> CsvParser::ParseToArrow(const ByteBuffer& data) const {
  std::string line(reinterpret_cast<const char*>(data.data()), data.size());
  StripLineEndings(&line);

  std::vector<std::string> fields;
  std::string err;
  if (!SplitCsvLine(line, opts_.delimiter, opts_.allow_quoted_fields, &fields, &err)) {
    return arrow::Status::Invalid(err);
  }

  // Infer schema if not provided: col0..colN as utf8.
  std::vector<FieldSchema> schema = input_schema_;
  if (schema.empty()) {
    schema.reserve(fields.size());
    for (size_t i = 0; i < fields.size(); ++i) {
      FieldSchema fs;
      fs.name = "col" + std::to_string(i);
      fs.type = "string";
      fs.field_number = static_cast<int>(i);
      schema.push_back(std::move(fs));
    }
  } else {
    if (fields.size() != schema.size()) {
      return arrow::Status::Invalid("CSV column count mismatch: got ", fields.size(),
                                   ", expected ", schema.size());
    }
  }

  std::vector<std::shared_ptr<arrow::Field>> arrow_fields;
  arrow_fields.reserve(schema.size());

  std::vector<std::shared_ptr<arrow::Array>> columns;
  columns.reserve(schema.size());

  for (size_t i = 0; i < schema.size(); ++i) {
    auto dt = FieldTypeFromString(schema[i].type);
    arrow_fields.push_back(arrow::field(schema[i].name, dt));

    ARROW_ASSIGN_OR_RAISE(auto sc, ParseScalar(fields[i], dt, opts_.empty_is_null));

    ARROW_ASSIGN_OR_RAISE(auto arr, arrow::MakeArrayFromScalar(*sc, /*length=*/1));
    columns.push_back(std::move(arr));
  }

  auto rb_schema = std::make_shared<arrow::Schema>(std::move(arrow_fields));
  return arrow::RecordBatch::Make(std::move(rb_schema), /*num_rows=*/1, std::move(columns));
} // namespace mycelium

}  // namespace mycelium