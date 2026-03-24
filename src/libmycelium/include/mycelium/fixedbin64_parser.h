#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "mycelium/transformer.h"

namespace mycelium {

struct FixedBin64RowPayload {
  std::vector<std::uint64_t> values;  // column values in order
};

class FixedBin64Parser final : public Parser {
 public:
  explicit FixedBin64Parser(int num_cols);

  InputOutputDataType InputType() const override;
  bool Validate(const ByteBuffer& input_data) const override;
  arrow::Result<ArrowRecord> ParseToArrow(const ByteBuffer& data) const override;
  const std::vector<FieldSchema>& GetInputFieldSchema() const override;

 private:
  int num_cols_;
  std::vector<FieldSchema> schema_;

  static std::uint64_t ReadFixed64LE(const std::uint8_t* p);
};

}  // namespace mycelium
