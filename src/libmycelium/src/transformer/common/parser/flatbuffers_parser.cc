#include "flatbuffers_parser.h"

#include <cstddef>
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

InputOutputDataType FlatbuffersParser::InputType() const {
  return InputOutputDataType::FLATBUFFERS;
} // namespace mycelium

bool FlatbuffersParser::Validate(const ByteBuffer& input_data) const {
  // Minimal check: non-empty. Replace with flatbuffers::Verifier if you can.
  return !input_data.empty();
} // namespace mycelium

namespace {

// Map FlatBuffers scalar base types to Arrow types.
arrow::Result<std::shared_ptr<arrow::DataType>>
ArrowTypeForBaseType(reflection::BaseType bt) {
  using BT = reflection::BaseType;
  switch (bt) {
    case BT::Bool:   return arrow::boolean();
    case BT::Byte:   return arrow::int8();
    case BT::UByte:  return arrow::uint8();
    case BT::Short:  return arrow::int16();
    case BT::UShort: return arrow::uint16();
    case BT::Int:    return arrow::int32();
    case BT::UInt:   return arrow::uint32();
    case BT::Long:   return arrow::int64();
    case BT::ULong:  return arrow::uint64();
    case BT::Float:  return arrow::float32();
    case BT::Double: return arrow::float64();
    case BT::String: return arrow::utf8();
    default:
      return arrow::Status::NotImplemented("FlatBuffers base type not supported: ", reflection::EnumNameBaseType(bt));
  }
} // namespace mycelium

arrow::Result<std::shared_ptr<arrow::DataType>>
ArrowTypeForField(const reflection::Schema& schema, const reflection::Field& f) {
  const auto* t = f.type();
  if (!t) return arrow::Status::Invalid("FlatBuffers field has null type");

  // Vectors become list<elem>.
  if (t->base_type() == reflection::Vector) {
    ARROW_ASSIGN_OR_RAISE(auto elem, ArrowTypeForBaseType(t->element()));
    return arrow::list(elem);
  }

  // Tables/structs (objects) can be made into struct<...> recursively, but that’s more work.
  // For now, we store nested objects as binary blobs or string; here we store binary.
  if (t->base_type() == reflection::Obj) {
    return arrow::binary();
  }

  return ArrowTypeForBaseType(t->base_type());
} // namespace mycelium

// Helpers to append a scalar field from a table.
arrow::Status AppendScalarFromTable(const flatbuffers::Table& tbl,
                                   const reflection::Field& f,
                                   arrow::ArrayBuilder* b) {
  const auto* t = f.type();
  const auto off = f.offset();

  // If the field is absent, append null.
  // For scalars in FlatBuffers, absence means "default value" conceptually; but in Arrow
  // you usually either store default or null. Here we choose NULL if not present.
  // If you prefer “default value”, change to append the default.
  if (!tbl.CheckField(off)) {
    return b->AppendNull();
  }

  switch (t->base_type()) {
    case reflection::Bool:
      return static_cast<arrow::BooleanBuilder*>(b)->Append(tbl.GetField<uint8_t>(off, 0) != 0);
    case reflection::Byte:
      return static_cast<arrow::Int8Builder*>(b)->Append(tbl.GetField<int8_t>(off, 0));
    case reflection::UByte:
      return static_cast<arrow::UInt8Builder*>(b)->Append(tbl.GetField<uint8_t>(off, 0));
    case reflection::Short:
      return static_cast<arrow::Int16Builder*>(b)->Append(tbl.GetField<int16_t>(off, 0));
    case reflection::UShort:
      return static_cast<arrow::UInt16Builder*>(b)->Append(tbl.GetField<uint16_t>(off, 0));
    case reflection::Int:
      return static_cast<arrow::Int32Builder*>(b)->Append(tbl.GetField<int32_t>(off, 0));
    case reflection::UInt:
      return static_cast<arrow::UInt32Builder*>(b)->Append(tbl.GetField<uint32_t>(off, 0));
    case reflection::Long:
      return static_cast<arrow::Int64Builder*>(b)->Append(tbl.GetField<int64_t>(off, 0));
    case reflection::ULong:
      return static_cast<arrow::UInt64Builder*>(b)->Append(tbl.GetField<uint64_t>(off, 0));
    case reflection::Float:
      return static_cast<arrow::FloatBuilder*>(b)->Append(tbl.GetField<float>(off, 0.0f));
    case reflection::Double:
      return static_cast<arrow::DoubleBuilder*>(b)->Append(tbl.GetField<double>(off, 0.0));
    case reflection::String: {
      auto s = tbl.GetPointer<const flatbuffers::String*>(off);
      if (!s) return b->AppendNull();
      return static_cast<arrow::StringBuilder*>(b)->Append(s->str());
    }
    case reflection::Obj: {
      // Nested object pointer. Store as binary: raw bytes of nested table.
      // Note: Without knowing object byte-length, we cannot slice exact bytes easily.
      // Pragmatic fallback: store NULL if present but not supported.
      // If you want nested objects, we can recursively build struct columns instead.
      return arrow::Status::NotImplemented("Nested object fields not yet implemented as columns: ", f.name()->str());
    }
    default:
      return arrow::Status::NotImplemented("Unsupported scalar base type: ", reflection::EnumNameBaseType(t->base_type()));
  }
} // namespace mycelium

arrow::Status AppendVectorFromTable(const flatbuffers::Table& tbl,
                                   const reflection::Field& f,
                                   arrow::ArrayBuilder* b) {
  const auto* t = f.type();
  const auto off = f.offset();

  auto* lb = static_cast<arrow::ListBuilder*>(b);
  arrow::ArrayBuilder* vb = lb->value_builder();

  // If vector absent, treat as NULL (not empty list).
  auto vec_ptr = tbl.GetPointer<const flatbuffers::Vector<uint8_t>*>(off);
  if (!vec_ptr) {
    return b->AppendNull();
  }

  ARROW_RETURN_NOT_OK(lb->Append()); // start list for this row

  // element types:
  switch (t->element()) {
    case reflection::Int: {
      auto v = tbl.GetPointer<const flatbuffers::Vector<int32_t>*>(off);
      if (!v) return arrow::Status::Invalid("Vector<int32_t> missing where expected");
      auto* bb = static_cast<arrow::Int32Builder*>(vb);
      for (uint32_t i = 0; i < v->size(); ++i) ARROW_RETURN_NOT_OK(bb->Append(v->Get(i)));
      return arrow::Status::OK();
    }
    case reflection::Long: {
      auto v = tbl.GetPointer<const flatbuffers::Vector<int64_t>*>(off);
      if (!v) return arrow::Status::Invalid("Vector<int64_t> missing where expected");
      auto* bb = static_cast<arrow::Int64Builder*>(vb);
      for (uint32_t i = 0; i < v->size(); ++i) ARROW_RETURN_NOT_OK(bb->Append(v->Get(i)));
      return arrow::Status::OK();
    }
    case reflection::UInt: {
      auto v = tbl.GetPointer<const flatbuffers::Vector<uint32_t>*>(off);
      if (!v) return arrow::Status::Invalid("Vector<uint32_t> missing where expected");
      auto* bb = static_cast<arrow::UInt32Builder*>(vb);
      for (uint32_t i = 0; i < v->size(); ++i) ARROW_RETURN_NOT_OK(bb->Append(v->Get(i)));
      return arrow::Status::OK();
    }
    case reflection::ULong: {
      auto v = tbl.GetPointer<const flatbuffers::Vector<uint64_t>*>(off);
      if (!v) return arrow::Status::Invalid("Vector<uint64_t> missing where expected");
      auto* bb = static_cast<arrow::UInt64Builder*>(vb);
      for (uint32_t i = 0; i < v->size(); ++i) ARROW_RETURN_NOT_OK(bb->Append(v->Get(i)));
      return arrow::Status::OK();
    }
    case reflection::Float: {
      auto v = tbl.GetPointer<const flatbuffers::Vector<float>*>(off);
      if (!v) return arrow::Status::Invalid("Vector<float> missing where expected");
      auto* bb = static_cast<arrow::FloatBuilder*>(vb);
      for (uint32_t i = 0; i < v->size(); ++i) ARROW_RETURN_NOT_OK(bb->Append(v->Get(i)));
      return arrow::Status::OK();
    }
    case reflection::Double: {
      auto v = tbl.GetPointer<const flatbuffers::Vector<double>*>(off);
      if (!v) return arrow::Status::Invalid("Vector<double> missing where expected");
      auto* bb = static_cast<arrow::DoubleBuilder*>(vb);
      for (uint32_t i = 0; i < v->size(); ++i) ARROW_RETURN_NOT_OK(bb->Append(v->Get(i)));
      return arrow::Status::OK();
    }
    case reflection::Bool: {
      // FlatBuffers uses uint8_t for bool vectors in memory.
      auto v = tbl.GetPointer<const flatbuffers::Vector<uint8_t>*>(off);
      if (!v) return arrow::Status::Invalid("Vector<bool> missing where expected");
      auto* bb = static_cast<arrow::BooleanBuilder*>(vb);
      for (uint32_t i = 0; i < v->size(); ++i) ARROW_RETURN_NOT_OK(bb->Append(v->Get(i) != 0));
      return arrow::Status::OK();
    }
    case reflection::String: {
      auto v = tbl.GetPointer<const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>*>(off);
      if (!v) return arrow::Status::Invalid("Vector<string> missing where expected");
      auto* bb = static_cast<arrow::StringBuilder*>(vb);
      for (uint32_t i = 0; i < v->size(); ++i) {
        auto s = v->Get(i);
        if (!s) ARROW_RETURN_NOT_OK(bb->AppendNull());
        else ARROW_RETURN_NOT_OK(bb->Append(s->str()));
      }
      return arrow::Status::OK();
    }
    default:
      return arrow::Status::NotImplemented("Vector element type unsupported: ", reflection::EnumNameBaseType(t->element()));
  }
} // namespace mycelium

} // namespace mycelium

arrow::Result<ArrowRecord> FlatbuffersParser::ParseToArrow(const ByteBuffer& data) const {
  if (!schema_ || !root_obj_) {
    return arrow::Status::Invalid("FlatbuffersParser::ParseToArrow: schema_ or root_obj_ is null");
  }

  // Optional: verify buffer is a valid instance of this root type.
  // You can keep a flag or always verify in debug.
  {
    flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    if (!v.VerifyBuffer<flat::Row>(nullptr)) {
      return arrow::Status::Invalid("FlatbuffersParser::ParseToArrow: flatbuffer verification failed");
    }
  }

  // Interpret root as a Table using reflection.
  const auto* root_tbl = flatbuffers::GetRoot<flatbuffers::Table>(
      reinterpret_cast<const uint8_t*>(data.data()));
  if (!root_tbl) {
    return arrow::Status::Invalid("FlatbuffersParser::ParseToArrow: GetRoot<Table> returned null");
  }

  // Build Arrow schema from reflection object fields.
  std::vector<std::shared_ptr<arrow::Field>> fields;
  fields.reserve(root_obj_->fields()->size());

  for (auto f_it = root_obj_->fields()->begin(); f_it != root_obj_->fields()->end(); ++f_it) {
    const reflection::Field* f = *f_it;
    ARROW_ASSIGN_OR_RAISE(auto dt, ArrowTypeForField(*schema_, *f));
    // FlatBuffers has “optional by omission” semantics; treat nullable=true broadly.
    fields.push_back(arrow::field(f->name()->str(), dt, /*nullable=*/true));
  }

  auto schema = arrow::schema(std::move(fields));

  // Builders.
  arrow::MemoryPool* pool = arrow::default_memory_pool();
  std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders;
  builders.reserve(schema->num_fields());
  for (int i = 0; i < schema->num_fields(); ++i) {
    std::unique_ptr<arrow::ArrayBuilder> b;
    ARROW_RETURN_NOT_OK(arrow::MakeBuilder(pool, schema->field(i)->type(), &b));
    builders.push_back(std::move(b));
  }

  // Append 1 row (each field becomes one column value).
  int col = 0;
  for (auto f_it = root_obj_->fields()->begin(); f_it != root_obj_->fields()->end(); ++f_it, ++col) {
    const reflection::Field* f = *f_it;
    const auto* t = f->type();
    if (t->base_type() == reflection::Vector) {
      ARROW_RETURN_NOT_OK(AppendVectorFromTable(*root_tbl, *f, builders[col].get()));
    } else {
      ARROW_RETURN_NOT_OK(AppendScalarFromTable(*root_tbl, *f, builders[col].get()));
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
