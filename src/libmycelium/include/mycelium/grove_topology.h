#pragma once

// mycelium/grove_topology.h
//
// Engine-agnostic description of an LSM-Grove: the set of derived trees that
// receive transformation output from a base tree.
//
// A GroveTopology is the portability-layer equivalent of the grove plan that
// MymBroker::buildPlan() produces inside the RocksDB adapter.  It carries:
//   - The transformer type assigned to this grove node.
//   - The names of the destination column families (or equivalent logical
//     containers in another engine).
//   - The number of output splits the encoder will produce (for DISTRIBUTOR).
//
// No RocksDB types are used here.  Engine-specific details (CF handles,
// ColumnFamilyOptions, …) live in the RocksDB adapter layer.

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace mycelium {

// Mirror of rocksdb::TransformerType — re-declared here so libmycelium has no
// dependency on rocksdb/transformer.h.  The numeric values MUST stay in sync.
enum class TransformerType : int {
  NOTRANSFORMATION = 0,
  DISTRIBUTOR      = 1 << 0,   // 1
  CONVERTER        = 1 << 1,   // 2
  AUGMENTER        = 1 << 2,   // 4
  MYNOOPER         = 1 << 3,   // 8
};

constexpr TransformerType operator|(TransformerType a, TransformerType b) {
  using T = int;
  return static_cast<TransformerType>(static_cast<T>(a) | static_cast<T>(b));
}
constexpr TransformerType operator&(TransformerType a, TransformerType b) {
  using T = int;
  return static_cast<TransformerType>(static_cast<T>(a) & static_cast<T>(b));
}
constexpr bool HasFlag(TransformerType mask, TransformerType flag) {
  return (mask & flag) == flag;
}

// ── GroveNode ─────────────────────────────────────────────────────────────────
// Describes a single transformation edge in the grove DAG: one transformer
// writing to one or more destination trees.
struct GroveNode {
  // The transformation type applied at this edge.
  TransformerType transformer_type = TransformerType::NOTRANSFORMATION;

  // Names of the destination column families (or equivalent logical
  // containers).  For a DISTRIBUTOR with N splits, len == N.
  // For a CONVERTER or AUGMENTER, len is typically 1.
  std::vector<std::string> dest_names;

  // Number of output records the encoder emits per input record.
  // Must match dest_names.size() for DISTRIBUTOR; usually 1 otherwise.
  int split_count = 1;

  // Human-readable label for logging / debugging.
  std::string label;
};

// ── GroveTopology ─────────────────────────────────────────────────────────────
// The full grove plan for one base column family: a list of GroveNodes that
// together describe every transformation to apply during compaction of that CF.
//
// Typical construction (from the RocksDB adapter):
//
//   GroveTopology topo;
//   topo.base_name = "default";
//   topo.nodes.push_back({ TransformerType::CONVERTER,
//                           {"default_converted"}, 1, "json→fb" });
//   topo.nodes.push_back({ TransformerType::AUGMENTER,
//                           {"index_cf"}, 1, "build-index" });
//
struct GroveTopology {
  // Name of the base column family this topology applies to.
  std::string base_name;

  // Ordered list of transformation nodes.  During compaction each node is
  // evaluated in order; the admission / scheduling layer may defer or skip
  // individual nodes.
  std::vector<GroveNode> nodes;

  // Convenience: collect all destination names across all nodes.
  std::vector<std::string> AllDestNames() const {
    std::vector<std::string> out;
    for (const auto& n : nodes)
      for (const auto& d : n.dest_names)
        out.push_back(d);
    return out;
  }

  bool Empty() const { return nodes.empty(); }
};

}  // namespace mycelium
