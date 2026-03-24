#pragma once
#include <vector>
#include "mycelium/transformer.h"

namespace mycelium {

// Augmenter: secondary-index transformer.
// For each configured set of column positions it extracts index-key tuples and
// emits them as additional ParsedRow records.
class Augmenter : public Transformer {
 public:
  explicit Augmenter(std::vector<std::vector<int>> ipositions)
      : index_positions_(std::move(ipositions)) {}
  ~Augmenter() override = default;

  std::string     Name()     const override { return "Augmenter"; }
  TransformerType Supports() const override { return TransformerType::AUGMENTER; }

  std::vector<ParsedRow> Transform(
      std::string_view  key,
      const ParsedRow&  input) const override;

  const std::vector<std::vector<int>>& GetPositionedIndexKeys() const {
    return index_positions_;
  }

 private:
  std::vector<std::vector<int>> index_positions_;
};

}  // namespace mycelium
