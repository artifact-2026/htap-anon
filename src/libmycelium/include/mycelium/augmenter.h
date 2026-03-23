#pragma once

#include <vector>

#include "mycelium/transformer.h"

namespace mycelium {

// Augmenter performs SECONDARY-INDEX transformation: given a set of column
// positions it extracts index-key tuples and emits them as additional records.
// data.pb.h is included only in the .cc; it is not part of the public interface.
class Augmenter : public Transformer {
 public:
  explicit Augmenter(std::vector<std::vector<int>> ipositions)
      : index_positions_(std::move(ipositions)) {}
  ~Augmenter() {}

  std::string Name() const override { return "Augmenter"; }
  TransformerType Supports() const override { return TransformerType::AUGMENTER; }

  std::vector<ArrowRecord> Transform(
      std::string_view key,
      const ArrowRecord& input) const override;

  const std::vector<std::vector<int>>& GetPositionedIndexKeys() const {
    return index_positions_;
  }

 private:
  std::vector<std::vector<int>> index_positions_;
};

}  // namespace mycelium
