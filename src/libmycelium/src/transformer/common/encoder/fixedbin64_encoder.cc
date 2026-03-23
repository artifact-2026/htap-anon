#include "fixedbin64_encoder.h"

#include <arrow/result.h>
#include <arrow/status.h>

#include <arrow/util/checked_cast.h>

namespace mycelium {

InputOutputDataType FixedBin64Encoder::OutputType() const {
  return InputOutputDataType::FIXEDBIN64;
} // namespace mycelium

bool ScalarToU64(const arrow::Scalar& s, std::uint64_t* out) {
  if (!s.is_valid) return false;

  switch (s.type->id()) {
    case arrow::Type::UINT8: {
      *out = static_cast<std::uint64_t>(
          arrow::internal::checked_cast<const arrow::UInt8Scalar&>(s).value);
      return true;
    }
    case arrow::Type::UINT16: {
      *out = static_cast<std::uint64_t>(
          arrow::internal::checked_cast<const arrow::UInt16Scalar&>(s).value);
      return true;
    }
    case arrow::Type::UINT32: {
      *out = static_cast<std::uint64_t>(
          arrow::internal::checked_cast<const arrow::UInt32Scalar&>(s).value);
      return true;
    }
    case arrow::Type::UINT64: {
      *out = arrow::internal::checked_cast<const arrow::UInt64Scalar&>(s).value;
      return true;
    }
    case arrow::Type::INT8: {
      const auto v = arrow::internal::checked_cast<const arrow::Int8Scalar&>(s).value;
      if (v < 0) return false;
      *out = static_cast<std::uint64_t>(v);
      return true;
    }
    case arrow::Type::INT16: {
      const auto v = arrow::internal::checked_cast<const arrow::Int16Scalar&>(s).value;
      if (v < 0) return false;
      *out = static_cast<std::uint64_t>(v);
      return true;
    }
    case arrow::Type::INT32: {
      const auto v = arrow::internal::checked_cast<const arrow::Int32Scalar&>(s).value;
      if (v < 0) return false;
      *out = static_cast<std::uint64_t>(v);
      return true;
    }
    case arrow::Type::INT64: {
      const auto v = arrow::internal::checked_cast<const arrow::Int64Scalar&>(s).value;
      if (v < 0) return false;
      *out = static_cast<std::uint64_t>(v);
      return true;
    }
    default:
      return false;
  }
} // namespace mycelium

void FixedBin64Encoder::AppendFixed64LE(ByteBuffer* out, std::uint64_t v) {
  out->push_back(static_cast<std::uint8_t>( v        & 0xFF));
  out->push_back(static_cast<std::uint8_t>((v >>  8) & 0xFF));
  out->push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
  out->push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
  out->push_back(static_cast<std::uint8_t>((v >> 32) & 0xFF));
  out->push_back(static_cast<std::uint8_t>((v >> 40) & 0xFF));
  out->push_back(static_cast<std::uint8_t>((v >> 48) & 0xFF));
  out->push_back(static_cast<std::uint8_t>((v >> 56) & 0xFF));
} // namespace mycelium

std::vector<ByteBuffer> FixedBin64Encoder::SerializeFromArrow(const ArrowRecord& rec) const {
  std::vector<ByteBuffer> out;

  if (!rec) return out;
  if (rec->num_rows() <= 0) return out;
  if (rec->num_columns() <= 0) return out;

  out.reserve(static_cast<size_t>(rec->num_rows()));

  for (int64_t i = 0; i < rec->num_rows(); ++i) {
    ByteBuffer values;
    values.reserve(static_cast<size_t>(rec->num_columns()) * 8);

    for (int c = 0; c < rec->num_columns(); ++c) {
      const auto& arr = rec->column(c);
      if (!arr || arr->IsNull(i)) {
        continue;
      }

      auto maybe_scalar = arr->GetScalar(i);
      if (!maybe_scalar.ok() || !*maybe_scalar) {
        continue;
      }
      const arrow::Scalar& s = **maybe_scalar;

      std::uint64_t v = 0;
      if (!ScalarToU64(s, &v)) {
        continue;
      }
      AppendFixed64LE(&values, v);
    }

    out.push_back(values);
  }

  return out;
} // namespace mycelium

}