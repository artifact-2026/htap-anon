#include "csv_encoder.h"

#include <arrow/util/checked_cast.h>

#include <string>
#include <memory>
#include <utility>

#include <arrow/scalar.h>
#include <arrow/type.h>
#include <arrow/util/checked_cast.h>
#include <arrow/result.h>
#include <arrow/status.h>

namespace mycelium {

InputOutputDataType CsvEncoder::OutputType() const {
  return InputOutputDataType::CSV;
} // namespace mycelium

std::string CsvEncoder::ScalarToStringForCsv(const arrow::Scalar& s) const {
  if (!s.is_valid) {
    return "";
  }

  switch (s.type->id()) {
    case arrow::Type::BOOL:
      return arrow::internal::checked_cast<const arrow::BooleanScalar&>(s).value ? "true" : "false";

    case arrow::Type::INT8:
      return std::to_string(arrow::internal::checked_cast<const arrow::Int8Scalar&>(s).value);
    case arrow::Type::INT16:
      return std::to_string(arrow::internal::checked_cast<const arrow::Int16Scalar&>(s).value);
    case arrow::Type::INT32:
      return std::to_string(arrow::internal::checked_cast<const arrow::Int32Scalar&>(s).value);
    case arrow::Type::INT64:
      return std::to_string(arrow::internal::checked_cast<const arrow::Int64Scalar&>(s).value);

    case arrow::Type::UINT8:
      return std::to_string(arrow::internal::checked_cast<const arrow::UInt8Scalar&>(s).value);
    case arrow::Type::UINT16:
      return std::to_string(arrow::internal::checked_cast<const arrow::UInt16Scalar&>(s).value);
    case arrow::Type::UINT32:
      return std::to_string(arrow::internal::checked_cast<const arrow::UInt32Scalar&>(s).value);
    case arrow::Type::UINT64:
      return std::to_string(arrow::internal::checked_cast<const arrow::UInt64Scalar&>(s).value);

    case arrow::Type::FLOAT:
      return std::to_string(arrow::internal::checked_cast<const arrow::FloatScalar&>(s).value);
    case arrow::Type::DOUBLE:
      return std::to_string(arrow::internal::checked_cast<const arrow::DoubleScalar&>(s).value);

    case arrow::Type::STRING:
      return arrow::internal::checked_cast<const arrow::StringScalar&>(s).ToString();
    case arrow::Type::LARGE_STRING:
      return arrow::internal::checked_cast<const arrow::LargeStringScalar&>(s).ToString();

    case arrow::Type::BINARY:
      // ToString() prints a readable representation; if you need base64/hex,
      // change this policy explicitly.
      return arrow::internal::checked_cast<const arrow::BinaryScalar&>(s).ToString();
    case arrow::Type::LARGE_BINARY:
      return arrow::internal::checked_cast<const arrow::LargeBinaryScalar&>(s).ToString();

    default:
      // Fallback: Arrow's Scalar::ToString() for types like decimal/date/timestamp, etc.
      // If you want to reject unsupported types instead, return "" and handle it in caller.
      return s.ToString();
  }
} // namespace mycelium

void CsvEncoder::AppendField(std::string* out, const std::string& f) {
  bool needs_quotes = false;
  for (char c : f) {
    if (c == '"' || c == ',' || c == '\n' || c == '\r') {
      needs_quotes = true;
      break;
    }
  }

  if (!needs_quotes) {
    out->append(f);
    return;
  }

  out->push_back('"');
  for (char c : f) {
    if (c == '"') {
      out->push_back('"');
      out->push_back('"');
    } else {
      out->push_back(c);
    }
  }
  out->push_back('"');
} // namespace mycelium

std::vector<ByteBuffer> CsvEncoder::SerializeFromArrow(const ArrowRecord& rec) const {
  std::vector<ByteBuffer> out;

  if (!rec) return out;
  if (rec->num_rows() <= 0) return out;
  if (rec->num_columns() <= 0) return out;

  out.reserve(static_cast<size_t>(rec->num_rows()));

  for (int64_t i = 0; i < rec->num_rows(); ++i) {
    std::string line;
    line.reserve(static_cast<size_t>(rec->num_columns()) * 10 + 1);

    for (int c = 0; c < rec->num_columns(); ++c) {
      if (c) line.push_back(',');

      const auto& arr = rec->column(c);
      if (!arr || arr->IsNull(i)) {
        continue;
      }

      auto maybe_scalar = arr->GetScalar(i);
      if (!maybe_scalar.ok() || !*maybe_scalar) {
        continue;
      }
      const arrow::Scalar& s = **maybe_scalar;

      std::string field_str = ScalarToStringForCsv(s);
      AppendField(&line, field_str);
    }

    line.push_back('\n');
    out.push_back(ByteBuffer(line.begin(), line.end()));
  }

  return out;
} // namespace mycelium

}