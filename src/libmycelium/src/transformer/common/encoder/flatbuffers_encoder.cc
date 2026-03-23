#include "flatbuffers_encoder.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <arrow/scalar.h>
#include <arrow/type.h>
#include <arrow/util/checked_cast.h>
#include <arrow/result.h>
#include <arrow/status.h>

#include <flatbuffers/flatbuffers.h> 

namespace mycelium {

InputOutputDataType FlatbuffersEncoder::OutputType() const {
  return InputOutputDataType::FLATBUFFERS;
} // namespace mycelium

std::vector<ByteBuffer> FlatbuffersEncoder::SerializeFromArrow(const ArrowRecord& rec) const {
  std::vector<ByteBuffer> out;

  if (!rec) return out;
  if (rec->num_rows() <= 0) return out;
  if (rec->num_columns() <= 0) return out;

  out.reserve(static_cast<size_t>(rec->num_rows()));

  for (int64_t i = 0; i < rec->num_rows(); ++i) {
    flatbuffers::FlatBufferBuilder fbb;
    std::vector<flatbuffers::Offset<flat::Column>> values;
    values.reserve(static_cast<size_t>(rec->num_columns()));

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

      switch (arr->type_id()) {
        case arrow::Type::STRING: {
          const auto& ss = arrow::internal::checked_cast<const arrow::StringScalar&>(s);
          const std::string& str = ss.value->ToString();
          auto bytes_off = fbb.CreateVector(
                              reinterpret_cast<const uint8_t*>(str.data()),
                              static_cast<flatbuffers::uoffset_t>(str.size()));
          values.push_back(flat::CreateColumn(fbb, bytes_off));
          break;
        }
        case arrow::Type::LARGE_STRING: {
          const auto& ss = arrow::internal::checked_cast<const arrow::LargeStringScalar&>(s);
          const std::string& str = ss.value->ToString();
          auto bytes_off = fbb.CreateVector(
                              reinterpret_cast<const uint8_t*>(str.data()),
                              static_cast<flatbuffers::uoffset_t>(str.size()));
          values.push_back(flat::CreateColumn(fbb, bytes_off));
          break;
        }
        case arrow::Type::INT32: {
          const auto& is = arrow::internal::checked_cast<const arrow::Int32Scalar&>(s);
          const int32_t v = is.value;
          std::array<uint8_t, 4> buf{
              static_cast<uint8_t>( static_cast<uint32_t>(v)        & 0xFF),
              static_cast<uint8_t>((static_cast<uint32_t>(v) >>  8) & 0xFF),
              static_cast<uint8_t>((static_cast<uint32_t>(v) >> 16) & 0xFF),
              static_cast<uint8_t>((static_cast<uint32_t>(v) >> 24) & 0xFF),
          };
          auto bytes_off = fbb.CreateVector(buf.data(),
                                   static_cast<flatbuffers::uoffset_t>(buf.size()));
          values.push_back(flat::CreateColumn(fbb, bytes_off));
          break;
        }
        case arrow::Type::DOUBLE: {
          const auto& ds = arrow::internal::checked_cast<const arrow::DoubleScalar&>(s);
          const double v = ds.value;
          std::array<uint8_t, 8> buf;
          std::memcpy(buf.data(), &v, 8);
          auto bytes_off = fbb.CreateVector(buf.data(),
                                   static_cast<flatbuffers::uoffset_t>(buf.size()));
          values.push_back(flat::CreateColumn(fbb, bytes_off));
          break;
        }
        default: {
          const std::string str = s.ToString();
          auto bytes_off = fbb.CreateVector(
                              reinterpret_cast<const uint8_t*>(str.data()),
                              static_cast<flatbuffers::uoffset_t>(str.size()));
          values.push_back(flat::CreateColumn(fbb, bytes_off));
          break;
        }
      }
    }

    auto vals_vec = fbb.CreateVector(values);
    auto row = flat::CreateRow(fbb, vals_vec);
    fbb.Finish(row);

    out.emplace_back(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
  }

  return out;
} // namespace mycelium

}  // namespace mycelium
