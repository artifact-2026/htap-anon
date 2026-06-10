#include "protobuf_parser.h"   // → mycelium/protobuf_parser.h

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>

namespace mycelium {

ProtobufParser::ProtobufParser(
    std::unique_ptr<google::protobuf::Message> template_message)
    : template_(std::move(template_message)) {}

InputOutputDataType ProtobufParser::InputType() const {
  return InputOutputDataType::PROTOBUF;
}

bool ProtobufParser::Validate(const ByteBuffer& input_data) const {
  if (!template_) return false;
  auto msg = std::unique_ptr<google::protobuf::Message>(template_->New());
  return msg->ParseFromArray(input_data.data(), static_cast<int>(input_data.size()));
}

namespace {

using FD = google::protobuf::FieldDescriptor;

// Convert one scalar (non-repeated) proto field to a FieldValue.
static FieldValue ProtoScalarToFieldValue(const google::protobuf::Message& msg,
                                          const FD* f) {
  const auto* refl = msg.GetReflection();

  bool has_value = true;
#if GOOGLE_PROTOBUF_VERSION >= 3021000
  if (f->has_presence()) has_value = refl->HasField(msg, f);
#else
  has_value = (f->cpp_type() == FD::CPPTYPE_MESSAGE) ? refl->HasField(msg, f) : true;
#endif
  if (!has_value) return FieldValue::MakeNull();

  switch (f->cpp_type()) {
    case FD::CPPTYPE_BOOL:   return FieldValue::MakeBool(refl->GetBool(msg, f));
    case FD::CPPTYPE_INT32:  return FieldValue::MakeInt32(refl->GetInt32(msg, f));
    case FD::CPPTYPE_INT64:  return FieldValue::MakeInt64(refl->GetInt64(msg, f));
    case FD::CPPTYPE_UINT32: return FieldValue::MakeUint32(refl->GetUInt32(msg, f));
    case FD::CPPTYPE_UINT64: return FieldValue::MakeUint64(refl->GetUInt64(msg, f));
    case FD::CPPTYPE_FLOAT:  return FieldValue::MakeFloat(refl->GetFloat(msg, f));
    case FD::CPPTYPE_DOUBLE: return FieldValue::MakeDouble(refl->GetDouble(msg, f));
    case FD::CPPTYPE_STRING: {
      const std::string& s = refl->GetStringReference(msg, f, nullptr);
      return FieldValue::MakeBytes(s);  // bytes and utf8 strings share Bytes kind
    }
    case FD::CPPTYPE_ENUM: {
      const auto* ev = refl->GetEnum(msg, f);
      return FieldValue::MakeBytes(ev ? ev->name() : "");
    }
    case FD::CPPTYPE_MESSAGE: {
      const auto& sub = refl->GetMessage(msg, f);
      std::string json;
      auto st = google::protobuf::util::MessageToJsonString(sub, &json);
      if (!st.ok()) return FieldValue::MakeNull();
      return FieldValue::MakeBytes(std::move(json));
    }
    default:
      return FieldValue::MakeNull();
  }
}

}  // namespace

Result<ParsedRow> ProtobufParser::Parse(const ByteBuffer& data) const {
  if (!template_)
    return Result<ParsedRow>::Err("ProtobufParser::Parse: template_ is null");

  auto msg = std::unique_ptr<google::protobuf::Message>(template_->New());
  if (!msg->ParseFromArray(data.data(), static_cast<int>(data.size())))
    return Result<ParsedRow>::Err("ProtobufParser::Parse: ParseFromArray failed");

  const auto* desc = msg->GetDescriptor();
  if (!desc)
    return Result<ParsedRow>::Err("ProtobufParser::Parse: message descriptor is null");

  ParsedRow row;
  row.fields.reserve(static_cast<size_t>(desc->field_count()));

  for (int i = 0; i < desc->field_count(); ++i) {
    const FD* f = desc->field(i);
    if (f->is_repeated()) {
      // Expand each element of a repeated field into its own ParsedField so
      // that the Distributor can split on individual elements (e.g. the YCSB
      // ByteRow.values repeated field where each element is one column).
      const auto* refl = msg->GetReflection();
      const int n = refl->FieldSize(*msg, f);
      for (int j = 0; j < n; ++j) {
        ParsedField pf;
        pf.name         = f->name() + "_" + std::to_string(j);
        pf.field_number = f->number();
        // Re-use the scalar helper by temporarily extracting the sub-message
        // or primitive via the repeated accessor.
        switch (f->cpp_type()) {
          case FD::CPPTYPE_BOOL:
            pf.value = FieldValue::MakeBool(refl->GetRepeatedBool(*msg, f, j)); break;
          case FD::CPPTYPE_INT32:
            pf.value = FieldValue::MakeInt32(refl->GetRepeatedInt32(*msg, f, j)); break;
          case FD::CPPTYPE_INT64:
            pf.value = FieldValue::MakeInt64(refl->GetRepeatedInt64(*msg, f, j)); break;
          case FD::CPPTYPE_UINT32:
            pf.value = FieldValue::MakeUint32(refl->GetRepeatedUInt32(*msg, f, j)); break;
          case FD::CPPTYPE_UINT64:
            pf.value = FieldValue::MakeUint64(refl->GetRepeatedUInt64(*msg, f, j)); break;
          case FD::CPPTYPE_FLOAT:
            pf.value = FieldValue::MakeFloat(refl->GetRepeatedFloat(*msg, f, j)); break;
          case FD::CPPTYPE_DOUBLE:
            pf.value = FieldValue::MakeDouble(refl->GetRepeatedDouble(*msg, f, j)); break;
          case FD::CPPTYPE_STRING: {
            const std::string& s = refl->GetRepeatedStringReference(*msg, f, j, nullptr);
            pf.value = FieldValue::MakeBytes(s);
            break;
          }
          case FD::CPPTYPE_ENUM: {
            const auto* ev = refl->GetRepeatedEnum(*msg, f, j);
            pf.value = FieldValue::MakeBytes(ev ? ev->name() : "");
            break;
          }
          case FD::CPPTYPE_MESSAGE: {
            const auto& sub = refl->GetRepeatedMessage(*msg, f, j);
            std::string json;
            auto st = google::protobuf::util::MessageToJsonString(sub, &json);
            pf.value = st.ok() ? FieldValue::MakeBytes(std::move(json))
                                : FieldValue::MakeNull();
            break;
          }
          default:
            pf.value = FieldValue::MakeNull(); break;
        }
        row.fields.push_back(std::move(pf));
      }
    } else {
      ParsedField pf;
      pf.name         = f->name();
      pf.field_number = f->number();
      pf.value        = ProtoScalarToFieldValue(*msg, f);
      row.fields.push_back(std::move(pf));
    }
  }

  return Result<ParsedRow>::Ok(std::move(row));
}

}  // namespace mycelium
