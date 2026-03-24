#pragma once
#include "mycelium/transformer.h"

namespace mycelium {

class FlatbuffersEncoder final : public Encoder {
 public:
  InputOutputDataType OutputType() const override;
  std::vector<ByteBuffer> Serialize(const ParsedRow& row) const override;
};

}  // namespace mycelium
