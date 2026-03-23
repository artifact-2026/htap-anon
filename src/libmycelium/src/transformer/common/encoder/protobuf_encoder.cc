#include "protobuf_encoder.h"

#include <cstdint>
#include <memory>

#include <arrow/scalar.h>
#include <arrow/type.h>
#include <arrow/util/checked_cast.h>
#include <arrow/result.h>
#include <arrow/status.h>

namespace mycelium {

namespace {

static inline void AppendVarint32(ByteBuffer& out, uint32_t v) {
  while (v >= 0x80) {
    out.push_back(static_cast<char>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<char>(v));
} // namespace mycelium

static inline void AppendVarint64(ByteBuffer& out, uint64_t v) {
  while (v >= 0x80) {
    out.push_back(static_cast<char>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<char>(v));
} // namespace mycelium

static inline uint32_t BytesFieldTag(uint32_t field_number) {
  // wire_type = 2 (length-delimited)
  return (field_number << 3) | 2u;
} // namespace mycelium

static inline bool ArrayValueToBytesView(const arrow::Array& arr,
                                        int64_t i,
                                        const char** data,
                                        size_t* len,
                                        char tmp_bool[1]) {
  if (arr.IsNull(i)) return false;  // caller decides what to do for nulls

  switch (arr.type_id()) {
    // ---- variable-length byte sequences ----
    case arrow::Type::BINARY: {
      const auto& a = static_cast<const arrow::BinaryArray&>(arr);
      int32_t sz = 0;
      const uint8_t* ptr = a.GetValue(i, &sz);   // ptr points to len bytes
      *data = reinterpret_cast<const char*>(ptr);
      *len = static_cast<size_t>(sz);
      return true;
    }
    case arrow::Type::STRING: {
      const auto& a = static_cast<const arrow::StringArray&>(arr);
      int32_t sz = 0;
      const uint8_t* ptr = a.GetValue(i, &sz);   // ptr points to len bytes
      *data = reinterpret_cast<const char*>(ptr);
      *len = static_cast<size_t>(sz);
      return true;
    }
    case arrow::Type::LARGE_BINARY: {
      const auto& a = static_cast<const arrow::LargeBinaryArray&>(arr);
      int64_t sz = 0;
      const uint8_t* ptr = a.GetValue(i, &sz);   // ptr points to len bytes
      *data = reinterpret_cast<const char*>(ptr);
      *len = static_cast<size_t>(sz);
      return true;
    }
    case arrow::Type::LARGE_STRING: {
      const auto& a = static_cast<const arrow::LargeStringArray&>(arr);
      int64_t sz = 0;
      const uint8_t* ptr = a.GetValue(i, &sz);   // ptr points to len bytes
      *data = reinterpret_cast<const char*>(ptr);
      *len = static_cast<size_t>(sz);
      return true;
    }

    // ---- fixed-size bytes ----
    case arrow::Type::FIXED_SIZE_BINARY: {
      const auto& a = static_cast<const arrow::FixedSizeBinaryArray&>(arr);
      const int32_t w = a.byte_width();
      *data = reinterpret_cast<const char*>(a.GetValue(i));
      *len = static_cast<size_t>(w);
      return true;
    }

    // ---- numeric primitives: serialize their in-memory bytes ----
    // WARNING: This encodes host-endian raw bytes. If you need portability,
    // define an explicit byte order (e.g., little-endian) and convert.
    case arrow::Type::INT8: {
      const auto& a = static_cast<const arrow::Int8Array&>(arr);
      *data = reinterpret_cast<const char*>(a.raw_values() + i);
      *len = sizeof(int8_t);
      return true;
    }
    case arrow::Type::UINT8: {
      const auto& a = static_cast<const arrow::UInt8Array&>(arr);
      *data = reinterpret_cast<const char*>(a.raw_values() + i);
      *len = sizeof(uint8_t);
      return true;
    }
    case arrow::Type::INT16: {
      const auto& a = static_cast<const arrow::Int16Array&>(arr);
      *data = reinterpret_cast<const char*>(a.raw_values() + i);
      *len = sizeof(int16_t);
      return true;
    }
    case arrow::Type::UINT16: {
      const auto& a = static_cast<const arrow::UInt16Array&>(arr);
      *data = reinterpret_cast<const char*>(a.raw_values() + i);
      *len = sizeof(uint16_t);
      return true;
    }
    case arrow::Type::INT32: {
      const auto& a = static_cast<const arrow::Int32Array&>(arr);
      *data = reinterpret_cast<const char*>(a.raw_values() + i);
      *len = sizeof(int32_t);
      return true;
    }
    case arrow::Type::UINT32: {
      const auto& a = static_cast<const arrow::UInt32Array&>(arr);
      *data = reinterpret_cast<const char*>(a.raw_values() + i);
      *len = sizeof(uint32_t);
      return true;
    }
    case arrow::Type::INT64: {
      const auto& a = static_cast<const arrow::Int64Array&>(arr);
      *data = reinterpret_cast<const char*>(a.raw_values() + i);
      *len = sizeof(int64_t);
      return true;
    }
    case arrow::Type::UINT64: {
      const auto& a = static_cast<const arrow::UInt64Array&>(arr);
      *data = reinterpret_cast<const char*>(a.raw_values() + i);
      *len = sizeof(uint64_t);
      return true;
    }
    case arrow::Type::FLOAT: {
      const auto& a = static_cast<const arrow::FloatArray&>(arr);
      *data = reinterpret_cast<const char*>(a.raw_values() + i);
      *len = sizeof(float);
      return true;
    }
    case arrow::Type::DOUBLE: {
      const auto& a = static_cast<const arrow::DoubleArray&>(arr);
      *data = reinterpret_cast<const char*>(a.raw_values() + i);
      *len = sizeof(double);
      return true;
    }

    // ---- boolean: Arrow stores bits; we materialize one byte ----
    case arrow::Type::BOOL: {
      const auto& a = static_cast<const arrow::BooleanArray&>(arr);
      tmp_bool[0] = a.Value(i) ? 1 : 0;
      *data = tmp_bool;
      *len = 1;
      return true;
    }

    default:
      return false;
  }
} // namespace mycelium

}  // namespace

std::vector<ByteBuffer> ProtobufBytesRowEncoder::SerializeFromArrow(const ArrowRecord& rec) const {
  std::vector<ByteBuffer> out;

  if (!rec) return out;
  if (rec->num_rows() <= 0) return out;
  if (rec->num_columns() <= 0) return out;

  out.reserve(static_cast<size_t>(rec->num_rows()));

  for (int64_t i = 0; i < rec->num_rows(); ++i) {
    ByteBuffer msg;
    msg.reserve(static_cast<size_t>(num_cols_) * 64);

    for (int c = 0; c < rec->num_columns(); ++c) {
      const auto& arr = rec->column(c);
      if (!arr || arr->IsNull(i)) {
        continue;
      }

      const char* data = nullptr;
      size_t len = 0;
      char tmp_bool[1];
      if (!ArrayValueToBytesView(*arr, i, &data, &len, tmp_bool)) continue;
        const uint32_t field_no = static_cast<uint32_t>(c + 1);
        AppendVarint32(msg, BytesFieldTag(field_no));
        AppendVarint64(msg, static_cast<uint64_t>(len));
        if (len > 0) {
          msg.insert(msg.end(), data, data + len);
        }
    }
    out.push_back(msg);
  }

  return out;
} // namespace mycelium

}  // namespace mycelium
