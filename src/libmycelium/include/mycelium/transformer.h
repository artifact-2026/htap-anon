// mycelium/transformer.h — portable transformer base (moved from rocksdb/transformer.h in P2)
#pragma once

#include <string>
#include <vector>
#include <map>
#include <type_traits>
#include <memory>
#include <typeindex>
#include <typeinfo>
#include <utility>

#include <string_view>

// rocksdb/slice.h intentionally NOT included here (P1: Slice removed from
// portable core).  The RocksDB adapter boundary uses Slice::ToStringView()
// before calling Transform().

#ifdef LZ4
  #pragma push_macro("LZ4")
  #undef LZ4
  #define ROCKSDB_RESTORE_LZ4_MACRO
#endif

#ifdef ZSTD
  #pragma push_macro("ZSTD")
  #undef ZSTD
  #define ROCKSDB_RESTORE_ZSTD_MACRO
#endif

#include <arrow/api.h>

#ifdef ROCKSDB_RESTORE_ZSTD_MACRO
  #pragma pop_macro("ZSTD")
  #undef ROCKSDB_RESTORE_ZSTD_MACRO
#endif

#ifdef ROCKSDB_RESTORE_LZ4_MACRO
  #pragma pop_macro("LZ4")
  #undef ROCKSDB_RESTORE_LZ4_MACRO
#endif

namespace mycelium {

/*
 *
 * TransformerType is a bitmask enum used to specify transformation behavior during compaction:
 * - DISTRIBUTOR: splits a record into multiple outputs written to separate column families
 * - CONVERTER: converts record formats (e.g., JSON → FlatBuffers)
 * - AUGMENTER: derives new records (e.g., index entries) in addition to writing the original
 * 
 * These types may be combined using bitwise OR to express compound transformations.
 * 
*/

enum class TransformerType {
  NOTRANSFORMATION     = 0,
  DISTRIBUTOR          = 1 << 0,       // 1
  CONVERTER            = 1 << 1,       // 2
  AUGMENTER            = 1 << 2,       // 4
  MYNOOPER             = 1 << 3,       // 8
};

enum class InputOutputDataType {
  UNKNOWN          = 0,
  JSON             = 1 << 0,
  PROTOBUF         = 1 << 1,
  FLATBUFFERS      = 1 << 2,
  AVRO             = 1 << 3,
  PARQUET          = 1 << 4,
  CSV              = 1 << 5,
  FIXEDBIN64       = 1 << 6,
  COLUMNBYTES      = 1 << 7,
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
  std::string type;  // Could use enum for strict typing if preferred
  int field_number;
};

using ArrowRecord = std::shared_ptr<arrow::RecordBatch>; 

class Parser {
 public:
  virtual ~Parser() = default;

  // Which on-the-wire format this parser accepts.
  virtual InputOutputDataType InputType() const = 0;

  // Optional quick validation (cheap checks).
  virtual bool Validate(const ByteBuffer& input_data) const { return true; }

  // Parse bytes into an in-memory representation.
  virtual  arrow::Result<ArrowRecord> ParseToArrow(const ByteBuffer& data) const = 0;

  // Optional: expose schema of the parsed representation (if you have it).
  virtual const std::vector<FieldSchema>& GetInputFieldSchema() const {
    static const std::vector<FieldSchema> kEmpty;
    return kEmpty;
  }
};

class Encoder {
 public:
  virtual ~Encoder() = default;

  // Which on-the-wire format this encoder produces.
  virtual InputOutputDataType OutputType() const = 0;

  // Encode an in-memory representation into bytes.
  virtual std::vector<ByteBuffer> SerializeFromArrow(const ArrowRecord& rec) const = 0;

  // Optional: describe outputs (useful for distributor/augmenter cases).
  virtual const std::vector<std::vector<FieldSchema>>& GetOutputFieldSchemas() const {
    static const std::vector<std::vector<FieldSchema>> kEmpty;
    return kEmpty;
  }
};

struct Codec {
  std::shared_ptr<Parser> parser;    // may be null
  std::shared_ptr<Encoder> encoder;  // may be null

  bool HasParser() const { return static_cast<bool>(parser); }
  bool HasEncoder() const { return static_cast<bool>(encoder); }
};

// A schema descriptor defines how to interpret or transform input data.
class SchemaDescriptor {
  public:
   ~SchemaDescriptor() = default;

   SchemaDescriptor(Codec in, Codec out,
                    std::vector<FieldSchema> input_schema,
                    std::vector<std::vector<FieldSchema>> output_schemas
                  ) : in_(std::move(in)), out_(std::move(out)), 
                      input_schema_(std::move(input_schema)),
                      output_schemas_(std::move(output_schemas)) {} 

   // Shows the data format before and after the transformation
   InputOutputDataType InputType() const {
    const auto& c = InputCodec();
    return c.parser ? c.parser->InputType() : InputOutputDataType::UNKNOWN;
   }
   InputOutputDataType OutputType() const {
    const auto& c = OutputCodec();
    return c.encoder ? c.encoder->OutputType() : InputOutputDataType::UNKNOWN;
   }
   bool Validate(const ByteBuffer& input_data) const {
    const auto& c = InputCodec();
    return c.parser ? c.parser->Validate(input_data) : true;
   } 

   arrow::Result<ArrowRecord> ParseToArrow(const ByteBuffer& data) const {
    const auto& c = InputCodec();
    if (!c.parser) return arrow::Status::Invalid("No parser configured");
    if (!c.parser->Validate(data)) return arrow::Status::Invalid("Validation failed");
    return c.parser->ParseToArrow(data);
   }
   arrow::Result<std::vector<ByteBuffer>> SerializeFromArrow(const ArrowRecord& rec) const {
    const auto& c = OutputCodec();
    if (!c.encoder) return arrow::Status::Invalid("No encoder configured");
    return c.encoder->SerializeFromArrow(rec);
   }

   const Codec& InputCodec() const {
    return in_;
   }
   const Codec& OutputCodec() const {
    return out_;
   }

   const std::vector<FieldSchema>& GetInputFieldSchema() const {
    return input_schema_;
   }
   const std::vector<std::vector<FieldSchema>>& GetOutputFieldSchemas() const {
    return output_schemas_;
   }

  private:
   Codec in_, out_;
   std::vector<FieldSchema> input_schema_;
   std::vector<std::vector<FieldSchema>> output_schemas_;
};

struct TransformContext {
  int splits;
  std::vector<std::vector<int>> index_positions;
};

class Transformer {
 public:
  virtual ~Transformer() = default;

  // Returns transformer name
  virtual std::string Name() const = 0;

  // Transforms a single input record into one or more outputs.
  // key is passed as string_view so the portable core has no Slice dependency.
  // At the RocksDB adapter boundary, convert with: slice.ToStringView()
  virtual std::vector<ArrowRecord> Transform(
      std::string_view key,
      const ArrowRecord& input) const = 0;
  
  // Declares which transformation features this transformer supports
  virtual TransformerType Supports() const = 0;

 private:
  TransformerType ttype_;
  TransformContext ctx_;
};

// Create a new Transformer that can be shared among multiple RocksDB instances
// Returns nullptr for TransformerType: NOTRANSFORMATION.
[[nodiscard]] std::shared_ptr<Transformer> CreateTransformer(
    const TransformerType transformer_type = TransformerType::NOTRANSFORMATION);

}