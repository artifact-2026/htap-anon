#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "mycelium/transformer.h"

namespace mycelium {

class JsonColsParser final : public Parser {
 public:
  explicit JsonColsParser(size_t num_cols, size_t expected_value_len = 0);

  InputOutputDataType InputType() const override { return InputOutputDataType::JSON; }

  bool Validate(const ByteBuffer& input_data) const override;

  arrow::Result<ArrowRecord> ParseToArrow(const ByteBuffer& data) const override;

  const std::vector<FieldSchema>& GetInputFieldSchema() const override { return input_schema_; }

 private:
  size_t num_cols_;
  size_t expected_value_len_;  // 0 => no check

  std::vector<FieldSchema> input_schema_;

  // Finds the quoted value for a given key "colK".
  // Returns false if not found.
  static bool ExtractStringValueForKey(const char* json,
                                      size_t n,
                                      const std::string& key,
                                      const char** out_begin,
                                      const char** out_end);

  static inline bool IsDigit(char c) { return c >= '0' && c <= '9'; }
};


}