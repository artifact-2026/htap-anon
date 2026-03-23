#pragma once

#include "mycelium/transformer.h"

namespace mycelium {

// Mynooper is the identity transformer: it passes every record through unchanged.
class Mynooper : public Transformer {
 public:
  Mynooper() {}
  ~Mynooper() {}

  std::string Name() const override { return "Mycelium-NoOp"; }
  TransformerType Supports() const override { return TransformerType::MYNOOPER; }

  std::vector<ArrowRecord> Transform(
      std::string_view key,
      const ArrowRecord& input) const override;
};

}  // namespace mycelium
