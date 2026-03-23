#pragma once

#include "mycelium/transformer.h"

#include <cstdint>
#include <string>
#include <vector>


namespace mycelium {

// Minimal parsed row representation for FIXEDBIN64.
// If you already have ParsedRow/ParsedFields elsewhere, use those instead.
struct FixedBin64RowPayload {
  std::vector<std::uint64_t> values;  // column values in order
};

class FixedBin64Parser final : public Parser {
 public:
  // num_cols is required because FIXEDBIN64 has no self-describing schema.
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

}