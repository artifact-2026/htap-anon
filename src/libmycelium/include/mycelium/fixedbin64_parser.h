#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "mycelium/transformer.h"

namespace mycelium {

class FixedBin64Parser final : public Parser {
 public:
  explicit FixedBin64Parser(int num_cols);

  InputOutputDataType InputType() const override;
  bool Validate(std::string_view input_data) const override;
  Result<ParsedRow> Parse(std::string_view data) const override;
  const std::vector<FieldSchema>& GetInputFieldSchema() const override;

 private:
  int num_cols_;
  std::vector<FieldSchema> schema_;

  static uint64_t ReadFixed64LE(const uint8_t* p);
};

}  // namespace mycelium
