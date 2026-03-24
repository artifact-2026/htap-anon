#include "protobuf_encoder.h"   // → mycelium/protobuf_encoder.h

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mycelium {

namespace {

static inline void AppendVarint32(ByteBuffer& out, uint32_t v) {
  while (v >= 0x80) {
    out.push_back(static_cast<uint8_t>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<uint8_t>(v));
}

static inline void AppendVarint64(ByteBuffer& out, uint64_t v) {
  while (v >= 0x80) {
    out.push_back(static_cast<uint8_t>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<uint8_t>(v));
}

static inline uint32_t BytesFieldTag(uint32_t field_number) {
  return (field_number << 3) | 2u;  // wire_type = 2 (length-delimited)
}

// Get the raw bytes representation of a FieldValue for protobuf encoding.
// Returns true and populates data/len (and uses tmp_str for numeric scratch).
static bool FieldValueToRawBytes(const FieldValue& fv,
                                  const char**      data,
                                  size_t*           len,
                                  std::string*      tmp_str) {
  if (fv.is_null()) return false;

  switch (fv.kind) {
    case FieldValue::Kind::Bytes:
      *data = fv.bytes.data();
      *len  = fv.bytes.size();
      return true;

    case FieldValue::Kind::Bool: {
      tmp_str->resize(1);
      (*tmp_str)[0] = fv.b ? '\x01' : '\x00';
      *data = tmp_str->data();
      *len  = 1;
      return true;
    }
    case FieldValue::Kind::Int32: {
      tmp_str->resize(sizeof(int32_t));
      std::memcpy(&(*tmp_str)[0], &fv.i32, sizeof(int32_t));
      *data = tmp_str->data();
      *len  = sizeof(int32_t);
      return true;
    }
    case FieldValue::Kind::Uint32: {
      tmp_str->resize(sizeof(uint32_t));
      std::memcpy(&(*tmp_str)[0], &fv.u32, sizeof(uint32_t));
      *data = tmp_str->data();
      *len  = sizeof(uint32_t);
      return true;
    }
    case FieldValue::Kind::Int64: {
      tmp_str->resize(sizeof(int64_t));
      std::memcpy(&(*tmp_str)[0], &fv.i64, sizeof(int64_t));
      *data = tmp_str->data();
      *len  = sizeof(int64_t);
      return true;
    }
    case FieldValue::Kind::Uint64: {
      tmp_str->resize(sizeof(uint64_t));
      std::memcpy(&(*tmp_str)[0], &fv.u64, sizeof(uint64_t));
      *data = tmp_str->data();
      *len  = sizeof(uint64_t);
      return true;
    }
    case FieldValue::Kind::Float: {
      tmp_str->resize(sizeof(float));
      std::memcpy(&(*tmp_str)[0], &fv.f32, sizeof(float));
      *data = tmp_str->data();
      *len  = sizeof(float);
      return true;
    }
    case FieldValue::Kind::Double: {
      tmp_str->resize(sizeof(double));
      std::memcpy(&(*tmp_str)[0], &fv.f64, sizeof(double));
      *data = tmp_str->data();
      *len  = sizeof(double);
      return true;
    }
    default:
      return false;  // List and Null
  }
}

}  // namespace

std::vector<ByteBuffer> ProtobufBytesRowEncoder::Serialize(const ParsedRow& row) const {
  if (row.empty()) return {};

  ByteBuffer msg;
  msg.reserve(row.size() * 64);

  std::string tmp_str;

  for (size_t c = 0; c < row.size(); ++c) {
    const FieldValue& fv = row.at(c).value;
    const char*       data = nullptr;
    size_t            len  = 0;

    if (!FieldValueToRawBytes(fv, &data, &len, &tmp_str)) continue;

    const uint32_t field_no = static_cast<uint32_t>(c + 1);
    AppendVarint32(msg, BytesFieldTag(field_no));
    AppendVarint64(msg, static_cast<uint64_t>(len));
    if (len > 0) msg.insert(msg.end(), data, data + len);
  }

  return {std::move(msg)};
}

}  // namespace mycelium
