#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mycelium/transformer.h"

namespace mycelium {

using SplitByPositions = std::vector<std::vector<int>>;

// Distributor does SPLIT transformation
class Distributor final : public Transformer {
 public:
  explicit Distributor(SplitByPositions pos) : splits_(std::move(pos)) {}

  std::string Name() const override { return "Distributor"; }
  TransformerType Supports() const override { return TransformerType::DISTRIBUTOR; }
  int GetNumSplits() const { return splits_.size(); }

  std::vector<ArrowRecord> Transform(
      std::string_view key,
      const ArrowRecord& input) const override;

 private:
  SplitByPositions splits_;
};

}  // namespace mycelium