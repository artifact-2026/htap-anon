#pragma once

#include "mycelium/transformer.h"

namespace mycelium {

// Converter performs FORMAT-CONVERSION transformation: translates the binary
// encoding of each record (e.g. FlatBuffers → Protobuf).  Generated-file
// headers (row_generated.h, data.pb.h) are included only in the .cc.
class Converter final : public Transformer {
 public:
  std::string Name() const override { return "convert_transformer"; }
  TransformerType Supports() const override { return TransformerType::CONVERTER; }

  std::vector<ArrowRecord> Transform(
      std::string_view key,
      const ArrowRecord& input) const override;
};

}  // namespace mycelium
