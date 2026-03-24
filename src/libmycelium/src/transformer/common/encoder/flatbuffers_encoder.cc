#include "flatbuffers_encoder.h"   // → mycelium/flatbuffers_encoder.h

#include <array>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#include <flatbuffers/flatbuffers.h>

// row_generated.h is included via the private header (flatbuffers_encoder.h
// re-exports from the implementation directory).
#include "row_generated.h"

namespace mycelium {

InputOutputDataType FlatbuffersEncoder::OutputType() const {
  return InputOutputDataType::FLATBUFFERS;
}

namespace {

// Convert a FieldValue to a raw byte sequence for use as a FlatBuffers column.
// Returns false (and leaves `out` unmodified) for Null / List values.
static bool FieldValueToBytes(const FieldValue& fv, std::vector<uint8_t>* out) {
  switch (fv.kind) {
    case FieldValue::Kind::Null:
      return false;
    case FieldValue::Kind::Bool: {
      uint8_t b = fv.b ? 1u : 0u;
      out->assign(1, b);
      return true;
    }
    case FieldValue::Kind::Int32: {
      uint32_t u;
      std::memcpy(&u, &fv.i32, 4);
      out->resize(4);
      out->at(0) = static_cast<uint8_t>( u        & 0xFF);
      out->at(1) = static_cast<uint8_t>((u >>  8) & 0xFF);
      out->at(2) = static_cast<uint8_t>((u >> 16) & 0xFF);
      out->at(3) = static_cast<uint8_t>((u >> 24) & 0xFF);
      return true;
    }
    case FieldValue::Kind::Uint32: {
      uint32_t u = fv.u32;
      out->resize(4);
      out->at(0) = static_cast<uint8_t>( u        & 0xFF);
      out->at(1) = static_cast<uint8_t>((u >>  8) & 0xFF);
      out->at(2) = static_cast<uint8_t>((u >> 16) & 0xFF);
      out->at(3) = static_cast<uint8_t>((u >> 24) & 0xFF);
      return true;
    }
    case FieldValue::Kind::Int64: {
      uint64_t u;
      std::memcpy(&u, &fv.i64, 8);
      out->resize(8);
      for (int i = 0; i < 8; ++i)
        (*out)[static_cast<size_t>(i)] = static_cast<uint8_t>((u >> (i*8)) & 0xFF);
      return true;
    }
    case FieldValue::Kind::Uint64: {
      uint64_t u = fv.u64;
      out->resize(8);
      for (int i = 0; i < 8; ++i)
        (*out)[static_cast<size_t>(i)] = static_cast<uint8_t>((u >> (i*8)) & 0xFF);
      return true;
    }
    case FieldValue::Kind::Float: {
      uint32_t u;
      std::memcpy(&u, &fv.f32, 4);
      out->resize(4);
      out->at(0) = static_cast<uint8_t>( u        & 0xFF);
      out->at(1) = static_cast<uint8_t>((u >>  8) & 0xFF);
      out->at(2) = static_cast<uint8_t>((u >> 16) & 0xFF);
      out->at(3) = static_cast<uint8_t>((u >> 24) & 0xFF);
      return true;
    }
    case FieldValue::Kind::Double: {
      uint64_t u;
      std::memcpy(&u, &fv.f64, 8);
      out->resize(8);
      for (int i = 0; i < 8; ++i)
        (*out)[static_cast<size_t>(i)] = static_cast<uint8_t>((u >> (i*8)) & 0xFF);
      return true;
    }
    case FieldValue::Kind::Bytes: {
      out->assign(fv.bytes.begin(), fv.bytes.end());
      return true;
    }
    case FieldValue::Kind::List:
      return false;  // lists not supported in FlatBuffers column encoding
  }
  return false;
}

}  // namespace

std::vector<ByteBuffer> FlatbuffersEncoder::Serialize(const ParsedRow& row) const {
  if (row.empty()) return {};

  flatbuffers::FlatBufferBuilder fbb;
  std::vector<flatbuffers::Offset<flat::Column>> values;
  values.reserve(row.size());

  std::vector<uint8_t> tmp;
  for (const auto& pf : row.fields) {
    tmp.clear();
    if (!FieldValueToBytes(pf.value, &tmp)) continue;
    auto bytes_off = fbb.CreateVector(tmp.data(),
                                       static_cast<flatbuffers::uoffset_t>(tmp.size()));
    values.push_back(flat::CreateColumn(fbb, bytes_off));
  }

  auto vals_vec = fbb.CreateVector(values);
  auto fb_row   = flat::CreateRow(fbb, vals_vec);
  fbb.Finish(fb_row);

  return {ByteBuffer(fbb.GetBufferPointer(),
                     fbb.GetBufferPointer() + fbb.GetSize())};
}

}  // namespace mycelium
