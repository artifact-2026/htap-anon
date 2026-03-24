#include "fixedbin64_encoder.h"   // → mycelium/fixedbin64_encoder.h

#include <cstdint>
#include <vector>

namespace mycelium {

InputOutputDataType FixedBin64Encoder::OutputType() const {
  return InputOutputDataType::FIXEDBIN64;
}

// static
void FixedBin64Encoder::AppendFixed64LE(ByteBuffer* out, uint64_t v) {
  out->push_back(static_cast<uint8_t>( v        & 0xFF));
  out->push_back(static_cast<uint8_t>((v >>  8) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 32) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 40) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 48) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 56) & 0xFF));
}

std::vector<ByteBuffer> FixedBin64Encoder::Serialize(const ParsedRow& row) const {
  if (row.empty()) return {};

  ByteBuffer values;
  values.reserve(row.size() * 8);

  for (size_t c = 0; c < row.size(); ++c) {
    const FieldValue& fv = row.at(c).value;
    uint64_t v = 0;

    switch (fv.kind) {
      case FieldValue::Kind::Uint64: v = fv.u64; break;
      case FieldValue::Kind::Uint32: v = fv.u32; break;
      case FieldValue::Kind::Int64:
        if (fv.i64 >= 0) v = static_cast<uint64_t>(fv.i64);
        else continue;
        break;
      case FieldValue::Kind::Int32:
        if (fv.i32 >= 0) v = static_cast<uint64_t>(fv.i32);
        else continue;
        break;
      default:
        continue;  // skip non-numeric / null fields
    }
    AppendFixed64LE(&values, v);
  }

  return {std::move(values)};
}

}  // namespace mycelium
