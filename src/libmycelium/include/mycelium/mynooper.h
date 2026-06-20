#pragma once
#include "mycelium/transformer.h"

namespace mycelium {

// Mynooper: identity transformer — passes every record through unchanged.
class Mynooper : public Transformer {
 public:
  Mynooper()  = default;
  ~Mynooper() override = default;

  std::string     Name()     const override { return "Mycelium-NoOp"; }
  TransformerType Supports() const override { return TransformerType::MYNOOPER; }

  std::vector<ParsedRow> Transform(
      std::string_view  key,
      const ParsedRow&  input) const override;

  std::vector<ParsedRow> TransformMove(
      std::string_view  key,
      ParsedRow&&       input) const override;
};

}  // namespace mycelium
