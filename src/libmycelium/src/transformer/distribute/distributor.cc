#include "distributor.h"   // → mycelium/distributor.h

#include <unordered_set>
#include <vector>

namespace mycelium {

namespace {
static bool ValidateSplitGroup(const std::vector<int>& cols, size_t num_fields) {
  if (cols.empty()) return false;
  std::unordered_set<int> seen;
  seen.reserve(cols.size());
  for (int c : cols) {
    if (c < 0 || static_cast<size_t>(c) >= num_fields) return false;
    if (!seen.insert(c).second) return false;   // duplicate within group
  }
  return true;
}
}  // namespace

std::vector<ParsedRow> Distributor::Transform(std::string_view /*key*/,
                                              const ParsedRow&  input) const {
  if (input.empty()) return {};

  std::vector<ParsedRow> outputs;
  outputs.reserve(splits_.size());

  for (const auto& cols : splits_) {
    if (!ValidateSplitGroup(cols, input.size())) return {};
    // SelectColumns copies only the requested fields — no Arrow overhead.
    outputs.push_back(input.SelectColumns(cols));
  }

  return outputs;
}

}  // namespace mycelium
