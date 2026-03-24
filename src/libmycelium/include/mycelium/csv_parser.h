#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "mycelium/transformer.h"

namespace arrow { class StructScalar; }

namespace mycelium {

struct CsvParserOptions {
  char delimiter;
  bool allow_quoted_fields;
  bool empty_is_null;  // true => empty field yields null instead of empty string / 0
};

class CsvParser final : public Parser {
 public:
  explicit CsvParser(std::vector<FieldSchema> input_schema = {},
                     CsvParserOptions opts = {});

  InputOutputDataType InputType() const override { return InputOutputDataType::CSV; }

  bool Validate(const ByteBuffer& input_data) const override;
  arrow::Result<ArrowRecord> ParseToArrow(const ByteBuffer& data) const override;
  const std::vector<FieldSchema>& GetInputFieldSchema() const override { return input_schema_; }

 private:
  std::vector<FieldSchema> input_schema_;
  CsvParserOptions opts_;
};

}  // namespace mycelium
