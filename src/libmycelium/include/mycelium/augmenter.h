#pragma once
#include <string_view>
#include <vector>
#include "mycelium/transformer.h"

namespace mycelium {

// Key separator embedded in every secondary-index CF entry.
// Index entry key format: field0%%field1%%...<kIndexKeySep><primary_key>
// Having this as a public constant lets deletion code (RocksDBGroveManager,
// MymBroker) perform suffix-scan deletes without hardcoding the string.
inline constexpr std::string_view kFieldSep    = "%%";
inline constexpr std::string_view kIndexKeySep = "$$$KEY$$$";

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
