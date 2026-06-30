#pragma once
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include "mycelium/transformer.h"

namespace mycelium {

using SplitByPositions = std::vector<std::vector<int>>;

// Distributor: one input record → N output records, one per destination CF,
// with columns selected by split_positions.
class Distributor final : public Transformer {
 public:
  explicit Distributor(SplitByPositions pos)
      : splits_(std::move(pos)), disjoint_(ComputeDisjoint()) {}

  std::string     Name()     const override { return "Distributor"; }
  TransformerType Supports() const override { return TransformerType::DISTRIBUTOR; }
  int             GetNumSplits() const { return static_cast<int>(splits_.size()); }

  std::vector<ParsedRow> Transform(
      std::string_view  key,
      const ParsedRow&  input) const override;

  // Move-based fast path: when splits are disjoint (the typical case), each
  // source field is moved into exactly one output row, eliminating all the
  // string copies that SelectColumns would otherwise make.  Falls back to the
  // copy-based Transform for overlapping splits.
  std::vector<ParsedRow> TransformMove(
      std::string_view key,
      ParsedRow&&      input) const override;

 private:
  SplitByPositions splits_;
  bool             disjoint_;  // true when no field index appears in >1 split

  // Returns true iff no column index appears in more than one split group.
  bool ComputeDisjoint() const {
    std::unordered_set<int> seen;
    for (const auto& cols : splits_) {
      for (int c : cols) {
        if (!seen.insert(c).second) return false;
      }
    }
    return true;
  }
};

}  // namespace mycelium
