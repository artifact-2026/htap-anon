#include "fixedbin64_parser.h"   // → mycelium/fixedbin64_parser.h

#include <cstdint>
#include <utility>

namespace mycelium {

FixedBin64Parser::FixedBin64Parser(int num_cols) : num_cols_(num_cols) {
  schema_.reserve(static_cast<size_t>(num_cols_));
  for (int i = 0; i < num_cols_; ++i) {
    FieldSchema fs;
    fs.name         = "col" + std::to_string(i);
    fs.type         = "uint64";
    fs.field_number = i;
    schema_.push_back(std::move(fs));
  }
}

InputOutputDataType FixedBin64Parser::InputType() const {
  return InputOutputDataType::FIXEDBIN64;
}

const std::vector<FieldSchema>& FixedBin64Parser::GetInputFieldSchema() const {
  return schema_;
}

bool FixedBin64Parser::Validate(const ByteBuffer& input_data) const {
  if (num_cols_ <= 0) return false;
  return input_data.size() == static_cast<size_t>(num_cols_) * 8;
}

// static
uint64_t FixedBin64Parser::ReadFixed64LE(const uint8_t* p) {
  return (static_cast<uint64_t>(p[0])      ) |
         (static_cast<uint64_t>(p[1]) <<  8) |
         (static_cast<uint64_t>(p[2]) << 16) |
         (static_cast<uint64_t>(p[3]) << 24) |
         (static_cast<uint64_t>(p[4]) << 32) |
         (static_cast<uint64_t>(p[5]) << 40) |
         (static_cast<uint64_t>(p[6]) << 48) |
         (static_cast<uint64_t>(p[7]) << 56);
}

Result<ParsedRow> FixedBin64Parser::Parse(const ByteBuffer& data) const {
  if (!Validate(data)) {
    return Result<ParsedRow>::Err(
        "FixedBin64Parser::Parse: invalid payload size; got=" +
        std::to_string(data.size()) +
        " expected=" + std::to_string(static_cast<size_t>(num_cols_) * 8));
  }

  ParsedRow row;
  row.fields.reserve(static_cast<size_t>(num_cols_));

  const uint8_t* p = data.data();
  for (int i = 0; i < num_cols_; ++i) {
    uint64_t v = ReadFixed64LE(p + static_cast<size_t>(i) * 8);
    ParsedField pf;
    pf.name         = schema_[static_cast<size_t>(i)].name;
    pf.field_number = schema_[static_cast<size_t>(i)].field_number;
    pf.value        = FieldValue::MakeUint64(v);
    row.fields.push_back(std::move(pf));
  }

  return Result<ParsedRow>::Ok(std::move(row));
}

}  // namespace mycelium
