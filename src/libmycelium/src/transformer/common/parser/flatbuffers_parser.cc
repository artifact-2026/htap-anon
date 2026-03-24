#include "flatbuffers_parser.h"   // → mycelium/flatbuffers_parser.h + row_generated.h

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mycelium {

bool FlatbuffersParser::Validate(const ByteBuffer& input_data) const {
  return !input_data.empty();
}

namespace {

// Read one scalar field from a FlatBuffers table into a FieldValue.
static FieldValue ReadScalarField(const flatbuffers::Table& tbl,
                                  const reflection::Field&  f) {
  const auto* t   = f.type();
  const auto  off = f.offset();

  if (!tbl.CheckField(off)) return FieldValue::MakeNull();

  switch (t->base_type()) {
    case reflection::Bool:
      return FieldValue::MakeBool(tbl.GetField<uint8_t>(off, 0) != 0);
    case reflection::Byte:
      return FieldValue::MakeInt32(tbl.GetField<int8_t>(off, 0));
    case reflection::UByte:
      return FieldValue::MakeUint32(tbl.GetField<uint8_t>(off, 0));
    case reflection::Short:
      return FieldValue::MakeInt32(tbl.GetField<int16_t>(off, 0));
    case reflection::UShort:
      return FieldValue::MakeUint32(tbl.GetField<uint16_t>(off, 0));
    case reflection::Int:
      return FieldValue::MakeInt32(tbl.GetField<int32_t>(off, 0));
    case reflection::UInt:
      return FieldValue::MakeUint32(tbl.GetField<uint32_t>(off, 0));
    case reflection::Long:
      return FieldValue::MakeInt64(tbl.GetField<int64_t>(off, 0));
    case reflection::ULong:
      return FieldValue::MakeUint64(tbl.GetField<uint64_t>(off, 0));
    case reflection::Float:
      return FieldValue::MakeFloat(tbl.GetField<float>(off, 0.0f));
    case reflection::Double:
      return FieldValue::MakeDouble(tbl.GetField<double>(off, 0.0));
    case reflection::String: {
      const auto* sv = tbl.GetPointer<const flatbuffers::String*>(off);
      if (!sv) return FieldValue::MakeNull();
      return FieldValue::MakeBytes(sv->str());
    }
    default:
      // Nested objects stored as null for now.
      return FieldValue::MakeNull();
  }
}

// Read a vector field from a FlatBuffers table into a FieldValue::List.
static FieldValue ReadVectorField(const flatbuffers::Table& tbl,
                                  const reflection::Field&  f) {
  const auto* t   = f.type();
  const auto  off = f.offset();

  auto vec_ptr = tbl.GetPointer<const flatbuffers::Vector<uint8_t>*>(off);
  if (!vec_ptr) return FieldValue::MakeNull();

  std::vector<FieldValue> elems;

  switch (t->element()) {
    case reflection::Bool: {
      auto v = tbl.GetPointer<const flatbuffers::Vector<uint8_t>*>(off);
      if (v) for (uint32_t i = 0; i < v->size(); ++i)
        elems.push_back(FieldValue::MakeBool(v->Get(i) != 0));
      break;
    }
    case reflection::Int: {
      auto v = tbl.GetPointer<const flatbuffers::Vector<int32_t>*>(off);
      if (v) for (uint32_t i = 0; i < v->size(); ++i)
        elems.push_back(FieldValue::MakeInt32(v->Get(i)));
      break;
    }
    case reflection::UInt: {
      auto v = tbl.GetPointer<const flatbuffers::Vector<uint32_t>*>(off);
      if (v) for (uint32_t i = 0; i < v->size(); ++i)
        elems.push_back(FieldValue::MakeUint32(v->Get(i)));
      break;
    }
    case reflection::Long: {
      auto v = tbl.GetPointer<const flatbuffers::Vector<int64_t>*>(off);
      if (v) for (uint32_t i = 0; i < v->size(); ++i)
        elems.push_back(FieldValue::MakeInt64(v->Get(i)));
      break;
    }
    case reflection::ULong: {
      auto v = tbl.GetPointer<const flatbuffers::Vector<uint64_t>*>(off);
      if (v) for (uint32_t i = 0; i < v->size(); ++i)
        elems.push_back(FieldValue::MakeUint64(v->Get(i)));
      break;
    }
    case reflection::Float: {
      auto v = tbl.GetPointer<const flatbuffers::Vector<float>*>(off);
      if (v) for (uint32_t i = 0; i < v->size(); ++i)
        elems.push_back(FieldValue::MakeFloat(v->Get(i)));
      break;
    }
    case reflection::Double: {
      auto v = tbl.GetPointer<const flatbuffers::Vector<double>*>(off);
      if (v) for (uint32_t i = 0; i < v->size(); ++i)
        elems.push_back(FieldValue::MakeDouble(v->Get(i)));
      break;
    }
    case reflection::String: {
      using StrVec = flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>;
      auto v = tbl.GetPointer<const StrVec*>(off);
      if (v) {
        for (uint32_t i = 0; i < v->size(); ++i) {
          auto sv = v->Get(i);
          elems.push_back(sv ? FieldValue::MakeBytes(sv->str()) : FieldValue::MakeNull());
        }
      }
      break;
    }
    default:
      break;
  }
  return FieldValue::MakeList(std::move(elems));
}

}  // namespace

Result<ParsedRow> FlatbuffersParser::Parse(const ByteBuffer& data) const {
  if (!schema_ || !root_obj_)
    return Result<ParsedRow>::Err(
        "FlatbuffersParser::Parse: schema_ or root_obj_ is null");

  {
    flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    if (!v.VerifyBuffer<flat::Row>(nullptr))
      return Result<ParsedRow>::Err(
          "FlatbuffersParser::Parse: flatbuffer verification failed");
  }

  const auto* root_tbl = flatbuffers::GetRoot<flatbuffers::Table>(
      reinterpret_cast<const uint8_t*>(data.data()));
  if (!root_tbl)
    return Result<ParsedRow>::Err(
        "FlatbuffersParser::Parse: GetRoot<Table> returned null");

  ParsedRow row;
  const auto* fields_vec = root_obj_->fields();
  if (fields_vec) row.fields.reserve(fields_vec->size());

  for (auto f_it = root_obj_->fields()->begin();
       f_it != root_obj_->fields()->end(); ++f_it) {
    const reflection::Field* f = *f_it;

    ParsedField pf;
    pf.name         = f->name()->str();
    pf.field_number = static_cast<int>(f->id());

    if (f->type()->base_type() == reflection::Vector) {
      pf.value = ReadVectorField(*root_tbl, *f);
    } else {
      pf.value = ReadScalarField(*root_tbl, *f);
    }

    row.fields.push_back(std::move(pf));
  }

  return Result<ParsedRow>::Ok(std::move(row));
}

}  // namespace mycelium
