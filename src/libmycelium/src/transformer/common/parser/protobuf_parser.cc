#include "protobuf_parser.h"

#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Arrow
#include <arrow/api.h>
#include <arrow/buffer.h>
#include <arrow/result.h>
#include <arrow/status.h>

namespace mycelium {

ProtobufParser::ProtobufParser(std::unique_ptr<google::protobuf::Message> template_message)
    : template_(std::move(template_message)) {}

InputOutputDataType ProtobufParser::InputType() const {
  return InputOutputDataType::PROTOBUF;
} // namespace mycelium

bool ProtobufParser::Validate(const rocksdb::ByteBuffer& input_data) const {
  if (!template_) return false;
  auto msg = std::unique_ptr<google::protobuf::Message>(template_->New());
  return msg->ParseFromArray(input_data.data(), static_cast<int>(input_data.size()));
} // namespace mycelium

namespace {
static inline bool IsProtoFieldNullable(const google::protobuf::FieldDescriptor* f) {
  // Proto3 scalars default to "present-but-default" semantics; presence is only
  // tracked for message fields and for proto3 optional scalars.
  if (f->is_repeated()) return true;                 // list can be empty; treat nullable ok
  if (f->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) return true;
#if GOOGLE_PROTOBUF_VERSION >= 3021000
  if (f->has_presence()) return true;                // proto2 optional/required, proto3 optional
#endif
  // For proto3 non-optional scalars, you can pick either:
  //  - nullable=false and always write a value (default if unset), or
  //  - nullable=true and write null when "unset" (but proto3 doesn't define unset).
  // We'll choose nullable=false for these scalars.
  return false;
} // namespace mycelium

static inline arrow::Result<std::shared_ptr<arrow::DataType>>
ArrowTypeForField(const google::protobuf::FieldDescriptor* f) {
  using FD = google::protobuf::FieldDescriptor;

  auto scalar_type = [&]() -> arrow::Result<std::shared_ptr<arrow::DataType>> {
    switch (f->cpp_type()) {
      case FD::CPPTYPE_BOOL:   return arrow::boolean();
      case FD::CPPTYPE_INT32:  return arrow::int32();
      case FD::CPPTYPE_INT64:  return arrow::int64();
      case FD::CPPTYPE_UINT32: return arrow::uint32();
      case FD::CPPTYPE_UINT64: return arrow::uint64();
      case FD::CPPTYPE_FLOAT:  return arrow::float32();
      case FD::CPPTYPE_DOUBLE: return arrow::float64();
      case FD::CPPTYPE_STRING:
        // Distinguish "string" vs "bytes"
        if (f->type() == FD::TYPE_BYTES) return arrow::binary();
        return arrow::utf8();
      case FD::CPPTYPE_ENUM:
        // Choose representation: int32 or string. String is often nicer for inspection.
        return arrow::utf8();
      case FD::CPPTYPE_MESSAGE:
        // Pragmatic default for nested messages: JSON string (one cell).
        // Alternative: arrow::binary() with SerializeAsString().
        return arrow::utf8();
      default:
        return arrow::Status::NotImplemented("Unsupported protobuf field type: ", f->full_name());
    }
  };

  ARROW_ASSIGN_OR_RAISE(auto t, scalar_type());
  if (f->is_repeated()) {
    return arrow::list(t);
  }
  return t;
} // namespace mycelium

static inline arrow::Result<std::unique_ptr<arrow::ArrayBuilder>>
MakeBuilderForType(const std::shared_ptr<arrow::DataType>& type,
                   arrow::MemoryPool* pool) {
  std::unique_ptr<arrow::ArrayBuilder> b;
  ARROW_RETURN_NOT_OK(arrow::MakeBuilder(pool, type, &b));
  return b;
} // namespace mycelium

// Append one scalar (non-repeated) field value.
static inline arrow::Status AppendScalarField(
    const google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor* f,
    arrow::ArrayBuilder* b) {

  using FD = google::protobuf::FieldDescriptor;
  const auto* refl = msg.GetReflection();

  // Presence handling
  bool has_value = true;
#if GOOGLE_PROTOBUF_VERSION >= 3021000
  if (f->has_presence()) {
    has_value = refl->HasField(msg, f);
  } else {
    // proto3 non-optional scalar: always treat as present (default value if unset).
    has_value = true;
  }
#else
  // Older protobuf: treat scalars as always present.
  has_value = (f->cpp_type() == FD::CPPTYPE_MESSAGE) ? refl->HasField(msg, f) : true;
#endif

  if (!has_value) {
    return b->AppendNull();
  }

  switch (f->cpp_type()) {
    case FD::CPPTYPE_BOOL:
      return static_cast<arrow::BooleanBuilder*>(b)->Append(refl->GetBool(msg, f));
    case FD::CPPTYPE_INT32:
      return static_cast<arrow::Int32Builder*>(b)->Append(refl->GetInt32(msg, f));
    case FD::CPPTYPE_INT64:
      return static_cast<arrow::Int64Builder*>(b)->Append(refl->GetInt64(msg, f));
    case FD::CPPTYPE_UINT32:
      return static_cast<arrow::UInt32Builder*>(b)->Append(refl->GetUInt32(msg, f));
    case FD::CPPTYPE_UINT64:
      return static_cast<arrow::UInt64Builder*>(b)->Append(refl->GetUInt64(msg, f));
    case FD::CPPTYPE_FLOAT:
      return static_cast<arrow::FloatBuilder*>(b)->Append(refl->GetFloat(msg, f));
    case FD::CPPTYPE_DOUBLE:
      return static_cast<arrow::DoubleBuilder*>(b)->Append(refl->GetDouble(msg, f));
    case FD::CPPTYPE_STRING: {
      const std::string& s = refl->GetStringReference(msg, f, nullptr);
      if (f->type() == FD::TYPE_BYTES) {
        return static_cast<arrow::BinaryBuilder*>(b)->Append(
            reinterpret_cast<const uint8_t*>(s.data()),
            static_cast<int32_t>(s.size()));
      }
      return static_cast<arrow::StringBuilder*>(b)->Append(s);
    }
    case FD::CPPTYPE_ENUM: {
      const auto* ev = refl->GetEnum(msg, f);
      // Store enum name (human readable). Swap to ev->number() if you want int32.
      return static_cast<arrow::StringBuilder*>(b)->Append(ev ? ev->name() : "");
    }
    case FD::CPPTYPE_MESSAGE: {
      const auto& sub = refl->GetMessage(msg, f);
      std::string json;
      auto st = google::protobuf::util::MessageToJsonString(sub, &json);
      if (!st.ok()) return arrow::Status::Invalid("MessageToJsonString failed for field: ", f->full_name());
      return static_cast<arrow::StringBuilder*>(b)->Append(json);
    }
    default:
      return arrow::Status::NotImplemented("Unsupported protobuf scalar field: ", f->full_name());
  }
} // namespace mycelium

// Append a repeated field as list<...> (one list value in this row).
static inline arrow::Status AppendRepeatedField(
    const google::protobuf::Message& msg,
    const google::protobuf::FieldDescriptor* f,
    arrow::ArrayBuilder* b) {

  using FD = google::protobuf::FieldDescriptor;
  const auto* refl = msg.GetReflection();

  auto* list_builder = static_cast<arrow::ListBuilder*>(b);
  ARROW_RETURN_NOT_OK(list_builder->Append()); // start a new list for this row
  arrow::ArrayBuilder* vb = list_builder->value_builder();

  const int n = refl->FieldSize(msg, f);

  switch (f->cpp_type()) {
    case FD::CPPTYPE_BOOL: {
      auto* bb = static_cast<arrow::BooleanBuilder*>(vb);
      for (int i = 0; i < n; ++i) ARROW_RETURN_NOT_OK(bb->Append(refl->GetRepeatedBool(msg, f, i)));
      return arrow::Status::OK();
    }
    case FD::CPPTYPE_INT32: {
      auto* bb = static_cast<arrow::Int32Builder*>(vb);
      for (int i = 0; i < n; ++i) ARROW_RETURN_NOT_OK(bb->Append(refl->GetRepeatedInt32(msg, f, i)));
      return arrow::Status::OK();
    }
    case FD::CPPTYPE_INT64: {
      auto* bb = static_cast<arrow::Int64Builder*>(vb);
      for (int i = 0; i < n; ++i) ARROW_RETURN_NOT_OK(bb->Append(refl->GetRepeatedInt64(msg, f, i)));
      return arrow::Status::OK();
    }
    case FD::CPPTYPE_UINT32: {
      auto* bb = static_cast<arrow::UInt32Builder*>(vb);
      for (int i = 0; i < n; ++i) ARROW_RETURN_NOT_OK(bb->Append(refl->GetRepeatedUInt32(msg, f, i)));
      return arrow::Status::OK();
    }
    case FD::CPPTYPE_UINT64: {
      auto* bb = static_cast<arrow::UInt64Builder*>(vb);
      for (int i = 0; i < n; ++i) ARROW_RETURN_NOT_OK(bb->Append(refl->GetRepeatedUInt64(msg, f, i)));
      return arrow::Status::OK();
    }
    case FD::CPPTYPE_FLOAT: {
      auto* bb = static_cast<arrow::FloatBuilder*>(vb);
      for (int i = 0; i < n; ++i) ARROW_RETURN_NOT_OK(bb->Append(refl->GetRepeatedFloat(msg, f, i)));
      return arrow::Status::OK();
    }
    case FD::CPPTYPE_DOUBLE: {
      auto* bb = static_cast<arrow::DoubleBuilder*>(vb);
      for (int i = 0; i < n; ++i) ARROW_RETURN_NOT_OK(bb->Append(refl->GetRepeatedDouble(msg, f, i)));
      return arrow::Status::OK();
    }
    case FD::CPPTYPE_STRING: {
      if (f->type() == FD::TYPE_BYTES) {
        auto* bb = static_cast<arrow::BinaryBuilder*>(vb);
        for (int i = 0; i < n; ++i) {
          const std::string& s = refl->GetRepeatedStringReference(msg, f, i, nullptr);
          ARROW_RETURN_NOT_OK(bb->Append(
              reinterpret_cast<const uint8_t*>(s.data()), static_cast<int32_t>(s.size())));
        }
        return arrow::Status::OK();
      } else {
        auto* bb = static_cast<arrow::StringBuilder*>(vb);
        for (int i = 0; i < n; ++i) {
          const std::string& s = refl->GetRepeatedStringReference(msg, f, i, nullptr);
          ARROW_RETURN_NOT_OK(bb->Append(s));
        }
        return arrow::Status::OK();
      }
    }
    case FD::CPPTYPE_ENUM: {
      auto* bb = static_cast<arrow::StringBuilder*>(vb);
      for (int i = 0; i < n; ++i) {
        const auto* ev = refl->GetRepeatedEnum(msg, f, i);
        ARROW_RETURN_NOT_OK(bb->Append(ev ? ev->name() : ""));
      }
      return arrow::Status::OK();
    }
    case FD::CPPTYPE_MESSAGE: {
      auto* bb = static_cast<arrow::StringBuilder*>(vb);
      for (int i = 0; i < n; ++i) {
        const auto& sub = refl->GetRepeatedMessage(msg, f, i);
        std::string json;
        auto st = google::protobuf::util::MessageToJsonString(sub, &json);
        if (!st.ok()) return arrow::Status::Invalid("MessageToJsonString failed for field: ", f->full_name());
        ARROW_RETURN_NOT_OK(bb->Append(json));
      }
      return arrow::Status::OK();
    }
    default:
      return arrow::Status::NotImplemented("Unsupported repeated field: ", f->full_name());
  }
} // namespace mycelium

} // namespace mycelium

arrow::Result<ArrowRecord> ProtobufParser::ParseToArrow(const rocksdb::ByteBuffer& data) const {
  if (!template_) {
    return arrow::Status::Invalid("ProtobufParser::ParseToArrow: template_ is null");
  }

  auto msg = std::unique_ptr<google::protobuf::Message>(template_->New());
  if (!msg->ParseFromArray(data.data(), static_cast<int>(data.size()))) {
    return arrow::Status::Invalid("ProtobufParser::ParseToArrow: ParseFromArray failed");
  }

  const auto* desc = msg->GetDescriptor();
  if (!desc) {
    return arrow::Status::Invalid("ProtobufParser::ParseToArrow: message descriptor is null");
  }

  // Build schema fields from protobuf descriptor.
  std::vector<std::shared_ptr<arrow::Field>> arrow_fields;
  arrow_fields.reserve(desc->field_count());

  for (int i = 0; i < desc->field_count(); ++i) {
    const auto* f = desc->field(i);
    ARROW_ASSIGN_OR_RAISE(auto dt, ArrowTypeForField(f));
    arrow_fields.push_back(arrow::field(f->name(), dt, /*nullable=*/IsProtoFieldNullable(f)));
  }

  auto schema = arrow::schema(std::move(arrow_fields));

  // Create builders for each column.
  arrow::MemoryPool* pool = arrow::default_memory_pool();
  std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders;
  builders.reserve(schema->num_fields());

  for (int i = 0; i < schema->num_fields(); ++i) {
    ARROW_ASSIGN_OR_RAISE(auto b, MakeBuilderForType(schema->field(i)->type(), pool));
    builders.push_back(std::move(b));
  }

  // Append 1 row.
  const auto* refl = msg->GetReflection();
  for (int i = 0; i < desc->field_count(); ++i) {
    const auto* f = desc->field(i);
    arrow::ArrayBuilder* b = builders[i].get();

    if (f->is_repeated()) {
      ARROW_RETURN_NOT_OK(AppendRepeatedField(*msg, f, b));
    } else {
      ARROW_RETURN_NOT_OK(AppendScalarField(*msg, f, b));
    }
  }

  // Finish arrays.
  std::vector<std::shared_ptr<arrow::Array>> arrays;
  arrays.reserve(builders.size());

  for (auto& b : builders) {
    std::shared_ptr<arrow::Array> arr;
    ARROW_RETURN_NOT_OK(b->Finish(&arr));
    arrays.push_back(std::move(arr));
  }

  return arrow::RecordBatch::Make(schema, /*num_rows=*/1, std::move(arrays));
} // namespace mycelium

}  // namespace mycelium
