// mycelium/transformer.h — portable transformer base
//
// Arrow has been removed from the per-record hot path.  The intermediate
// representation is now ParsedRow / FieldValue, a lightweight row type that
// carries no Arrow allocator overhead when processing one record at a time.
//
// If batch-level Arrow interop is needed in the future, a thin adapter from
// ParsedRow → arrow::RecordBatch can be added at the output boundary without
// touching this interface.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "mycelium/status.h"

namespace mycelium {

// ── TransformerType ──────────────────────────────────────────────────────────

/*
 * TransformerType is a bitmask enum used to specify transformation behaviour
 * during compaction:
 *   DISTRIBUTOR : splits a record into multiple outputs for separate CFs
 *   CONVERTER   : converts record formats (e.g. JSON → FlatBuffers)
 *   AUGMENTER   : derives new records (e.g. index entries) alongside the original
 *
 * Types may be combined with bitwise OR for compound transformations.
 */
enum class TransformerType {
  NOTRANSFORMATION = 0,
  DISTRIBUTOR      = 1 << 0,   // 1
  CONVERTER        = 1 << 1,   // 2
  AUGMENTER        = 1 << 2,   // 4
  MYNOOPER         = 1 << 3,   // 8
};

enum class InputOutputDataType {
  UNKNOWN      = 0,
  JSON         = 1 << 0,
  PROTOBUF     = 1 << 1,
  FLATBUFFERS  = 1 << 2,
  AVRO         = 1 << 3,
  PARQUET      = 1 << 4,
  CSV          = 1 << 5,
  FIXEDBIN64   = 1 << 6,
  COLUMNBYTES  = 1 << 7,
};

constexpr TransformerType operator|(TransformerType lhs, TransformerType rhs) {
  using T = std::underlying_type_t<TransformerType>;
  return static_cast<TransformerType>(static_cast<T>(lhs) | static_cast<T>(rhs));
}
constexpr TransformerType operator&(TransformerType lhs, TransformerType rhs) {
  using T = std::underlying_type_t<TransformerType>;
  return static_cast<TransformerType>(static_cast<T>(lhs) & static_cast<T>(rhs));
}
constexpr int to_underlying(TransformerType type) {
  return static_cast<std::underlying_type_t<TransformerType>>(type);
}

using ByteBuffer = std::vector<uint8_t>;

struct FieldSchema {
  std::string name;
  std::string type;      // "string", "int64", "uint64", "double", "bool", "binary", …
  int         field_number = 0;
};

// ── Result<T> ────────────────────────────────────────────────────────────────
// Minimal Result type so parsers can return either a value or an error without
// depending on arrow::Result or std::expected (C++23).
template <typename T>
struct Result {
  T      value{};
  Status status;

  bool ok()            const noexcept { return status.ok(); }
  T&       operator*()       noexcept { return value; }
  const T& operator*() const noexcept { return value; }

  static Result<T> Ok(T v) {
    return Result<T>{std::move(v), Status::OK()};
  }
  static Result<T> Err(std::string msg) {
    return Result<T>{T{}, Status::Error(std::move(msg))};
  }
};

// ── FieldValue ────────────────────────────────────────────────────────────────
// Typed value for one field in a parsed record.
//
// Numeric widening policy (keeps the struct simple, encoders recover exact type):
//   int8 / int16 / int32  → Kind::Int32 or Kind::Int64 (parser's choice)
//   uint8 / uint16 / uint32 → Kind::Uint32 or Kind::Uint64
//   float                 → Kind::Float (preserved, no double-widening)
//
struct FieldValue {
  enum class Kind {
    Null,
    Bool,
    Int32,
    Int64,
    Uint32,
    Uint64,
    Float,
    Double,
    Bytes,   // raw binary bytes or UTF-8 string; stored in `bytes`
    List,    // repeated field; stored in `list`
  };

  Kind     kind = Kind::Null;

  bool     b   = false;
  int32_t  i32 = 0;
  int64_t  i64 = 0;
  uint32_t u32 = 0;
  uint64_t u64 = 0;
  float    f32 = 0.0f;
  double   f64 = 0.0;
  std::string            bytes;  // kind == Bytes
  std::vector<FieldValue> list;  // kind == List

  bool is_null() const noexcept { return kind == Kind::Null; }

  // Human-readable representation (used for index key building, CSV, debug).
  std::string ToString() const {
    switch (kind) {
      case Kind::Null:   return "";
      case Kind::Bool:   return b ? "true" : "false";
      case Kind::Int32:  return std::to_string(i32);
      case Kind::Int64:  return std::to_string(i64);
      case Kind::Uint32: return std::to_string(u32);
      case Kind::Uint64: return std::to_string(u64);
      case Kind::Float:  return std::to_string(f32);
      case Kind::Double: return std::to_string(f64);
      case Kind::Bytes:  return bytes;
      case Kind::List: {
        std::string out = "[";
        for (size_t i = 0; i < list.size(); ++i) {
          if (i) out += ",";
          out += list[i].ToString();
        }
        return out + "]";
      }
    }
    return "";
  }

  // ── Factories ──────────────────────────────────────────────────────────────
  static FieldValue MakeNull()              { return FieldValue{}; }
  static FieldValue MakeBool(bool v)        { FieldValue f; f.kind=Kind::Bool;   f.b=v;   return f; }
  static FieldValue MakeInt32(int32_t v)    { FieldValue f; f.kind=Kind::Int32;  f.i32=v; return f; }
  static FieldValue MakeInt64(int64_t v)    { FieldValue f; f.kind=Kind::Int64;  f.i64=v; return f; }
  static FieldValue MakeUint32(uint32_t v)  { FieldValue f; f.kind=Kind::Uint32; f.u32=v; return f; }
  static FieldValue MakeUint64(uint64_t v)  { FieldValue f; f.kind=Kind::Uint64; f.u64=v; return f; }
  static FieldValue MakeFloat(float v)      { FieldValue f; f.kind=Kind::Float;  f.f32=v; return f; }
  static FieldValue MakeDouble(double v)    { FieldValue f; f.kind=Kind::Double; f.f64=v; return f; }
  static FieldValue MakeBytes(std::string s){ FieldValue f; f.kind=Kind::Bytes;  f.bytes=std::move(s); return f; }
  static FieldValue MakeList(std::vector<FieldValue> l) {
    FieldValue f; f.kind=Kind::List; f.list=std::move(l); return f;
  }
};

// ── ParsedField / ParsedRow ──────────────────────────────────────────────────
// One named field in a parsed record.
struct ParsedField {
  std::string name;
  int         field_number = 0;   // mirrors FieldSchema::field_number
  FieldValue  value;
};

// Ordered list of named fields representing one parsed record.
// This is the lightweight intermediate type replacing arrow::RecordBatch.
struct ParsedRow {
  std::vector<ParsedField> fields;

  size_t size()  const noexcept { return fields.size(); }
  bool   empty() const noexcept { return fields.empty(); }

  const ParsedField& at(size_t i) const { return fields[i]; }
        ParsedField& at(size_t i)       { return fields[i]; }

  // Return a new row containing only the columns at the given indices.
  // Used by Distributor to select column subsets without copying values.
  ParsedRow SelectColumns(const std::vector<int>& indices) const {
    ParsedRow out;
    out.fields.reserve(indices.size());
    for (int idx : indices) {
      if (idx >= 0 && static_cast<size_t>(idx) < fields.size()) {
        out.fields.push_back(fields[static_cast<size_t>(idx)]);
      }
    }
    return out;
  }

  // Move-based variant: transfers field ownership to the new row.
  // Safe when each source field index appears in at most one call (disjoint
  // splits). Moved-from slots are left in a valid but unspecified state.
  ParsedRow MoveColumns(const std::vector<int>& indices) {
    ParsedRow out;
    out.fields.reserve(indices.size());
    for (int idx : indices) {
      if (idx >= 0 && static_cast<size_t>(idx) < fields.size()) {
        out.fields.push_back(std::move(fields[static_cast<size_t>(idx)]));
      }
    }
    return out;
  }
};

// ── Parser ───────────────────────────────────────────────────────────────────
class Parser {
 public:
  virtual ~Parser() = default;

  // Which on-the-wire format this parser accepts.
  virtual InputOutputDataType InputType() const = 0;

  // Optional cheap validation pass.
  virtual bool Validate(const ByteBuffer& input_data) const { return !input_data.empty(); }

  // Parse bytes into a ParsedRow.  Returns Err on malformed input.
  virtual Result<ParsedRow> Parse(const ByteBuffer& data) const = 0;

  // Optional: expose schema of the parsed representation.
  virtual const std::vector<FieldSchema>& GetInputFieldSchema() const {
    static const std::vector<FieldSchema> kEmpty;
    return kEmpty;
  }
};

// ── Encoder ──────────────────────────────────────────────────────────────────
class Encoder {
 public:
  virtual ~Encoder() = default;

  // Which on-the-wire format this encoder produces.
  virtual InputOutputDataType OutputType() const = 0;

  // Encode a ParsedRow into one or more byte buffers (one per output record).
  virtual std::vector<ByteBuffer> Serialize(const ParsedRow& row) const = 0;

  // Optional: describe outputs (useful for distributor/augmenter cases).
  virtual const std::vector<std::vector<FieldSchema>>& GetOutputFieldSchemas() const {
    static const std::vector<std::vector<FieldSchema>> kEmpty;
    return kEmpty;
  }
};

// ── Codec ────────────────────────────────────────────────────────────────────
struct Codec {
  std::shared_ptr<Parser>  parser;   // may be null
  std::shared_ptr<Encoder> encoder;  // may be null

  bool HasParser()  const { return static_cast<bool>(parser); }
  bool HasEncoder() const { return static_cast<bool>(encoder); }
};

// ── SchemaDescriptor ─────────────────────────────────────────────────────────
// Combines an input codec (parser) and output codec (encoder) with schema
// metadata.  This is the object registered on ColumnFamilyOptions.
class SchemaDescriptor {
 public:
  ~SchemaDescriptor() = default;

  SchemaDescriptor(Codec in, Codec out,
                   std::vector<FieldSchema>              input_schema,
                   std::vector<std::vector<FieldSchema>> output_schemas)
      : in_(std::move(in)), out_(std::move(out)),
        input_schema_(std::move(input_schema)),
        output_schemas_(std::move(output_schemas)) {}

  InputOutputDataType InputType() const {
    return in_.parser ? in_.parser->InputType() : InputOutputDataType::UNKNOWN;
  }
  InputOutputDataType OutputType() const {
    return out_.encoder ? out_.encoder->OutputType() : InputOutputDataType::UNKNOWN;
  }
  bool Validate(const ByteBuffer& data) const {
    return in_.parser ? in_.parser->Validate(data) : true;
  }

  // Parse raw bytes → ParsedRow using the input codec's parser.
  Result<ParsedRow> Parse(const ByteBuffer& data) const {
    if (!in_.parser) return Result<ParsedRow>::Err("No parser configured");
    if (!in_.parser->Validate(data))
      return Result<ParsedRow>::Err("Validation failed");
    return in_.parser->Parse(data);
  }

  // Serialize ParsedRow → byte buffers using the output codec's encoder.
  Result<std::vector<ByteBuffer>> Serialize(const ParsedRow& row) const {
    if (!out_.encoder)
      return Result<std::vector<ByteBuffer>>::Err("No encoder configured");
    return Result<std::vector<ByteBuffer>>::Ok(out_.encoder->Serialize(row));
  }

  const Codec& InputCodec()  const { return in_; }
  const Codec& OutputCodec() const { return out_; }

  const std::vector<FieldSchema>&              GetInputFieldSchema()  const { return input_schema_; }
  const std::vector<std::vector<FieldSchema>>& GetOutputFieldSchemas() const { return output_schemas_; }

 private:
  Codec in_, out_;
  std::vector<FieldSchema>              input_schema_;
  std::vector<std::vector<FieldSchema>> output_schemas_;
};

// ── TransformContext / Transformer ───────────────────────────────────────────
struct TransformContext {
  int splits;
  std::vector<std::vector<int>> index_positions;
};

class Transformer {
 public:
  virtual ~Transformer() = default;

  virtual std::string Name() const = 0;

  // Transform one input ParsedRow into zero or more output ParsedRows.
  // key is passed as string_view (no Slice dependency in portable core).
  virtual std::vector<ParsedRow> Transform(
      std::string_view    key,
      const ParsedRow&    input) const = 0;

  // Move-based variant: callers that own the input and do not need it after
  // this call should prefer this overload so implementations can steal field
  // data instead of copying it.  The default falls back to the copy-based
  // Transform; subclasses override for efficiency.
  virtual std::vector<ParsedRow> TransformMove(
      std::string_view key,
      ParsedRow&&      input) const {
    return Transform(key, input);
  }

  // Declares which transformation features this transformer supports.
  virtual TransformerType Supports() const = 0;

 private:
  TransformerType  ttype_{};
  TransformContext ctx_{};
};

// Factory function — returns nullptr for NOTRANSFORMATION.
[[nodiscard]] std::shared_ptr<Transformer> CreateTransformer(
    TransformerType transformer_type = TransformerType::NOTRANSFORMATION);

}  // namespace mycelium
