#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "mycelium/transformer.h"

namespace mycelium {

struct CsvParserOptions {
  char delimiter           = ',';
  bool allow_quoted_fields = true;
  bool empty_is_null       = false;
};

class CsvParser final : public Parser {
 public:
  explicit CsvParser(std::vector<FieldSchema> input_schema = {},
                     CsvParserOptions opts = {});

  InputOutputDataType InputType() const override { return InputOutputDataType::CSV; }

  bool Validate(const ByteBuffer& input_data) const override;
  Result<ParsedRow> Parse(const ByteBuffer& data) const override;
  const std::vector<FieldSchema>& GetInputFieldSchema() const override { return input_schema_; }

 private:
  std::vector<FieldSchema> input_schema_;
  CsvParserOptions         opts_;
};

}  // namespace mycelium
