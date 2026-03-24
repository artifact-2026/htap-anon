#pragma once
#include <string>
#include <utility>
#include <vector>
#include "mycelium/transformer.h"

namespace mycelium {

using SplitByPositions = std::vector<std::vector<int>>;

// Distributor: one input record → N output records, one per destination CF,
// with columns selected by split_positions.
class Distributor final : public Transformer {
 public:
  explicit Distributor(SplitByPositions pos) : splits_(std::move(pos)) {}

  std::string     Name()     const override { return "Distributor"; }
  TransformerType Supports() const override { return TransformerType::DISTRIBUTOR; }
  int             GetNumSplits() const { return static_cast<int>(splits_.size()); }

  std::vector<ParsedRow> Transform(
      std::string_view  key,
      const ParsedRow&  input) const override;

 private:
  SplitByPositions splits_;
};

}  // namespace mycelium
