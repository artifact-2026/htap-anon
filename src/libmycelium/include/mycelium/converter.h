#pragma once
#include "mycelium/transformer.h"

namespace mycelium {

// Converter: format-conversion transformer.
// Passes the ParsedRow through unchanged; the SchemaDescriptor's output encoder
// re-serializes it in the target format.
class Converter final : public Transformer {
 public:
  std::string     Name()     const override { return "convert_transformer"; }
  TransformerType Supports() const override { return TransformerType::CONVERTER; }

  std::vector<ParsedRow> Transform(
      std::string_view  key,
      const ParsedRow&  input) const override;
};

}  // namespace mycelium
