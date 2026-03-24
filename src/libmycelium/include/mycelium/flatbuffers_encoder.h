#pragma once
// FlatbuffersEncoder serialises an ArrowRecord to a FlatBuffers byte buffer.
// row_generated.h and flatbuffers_parser.h are implementation details; they
// are NOT included here.  The .cc includes the private header for those.
#include "mycelium/transformer.h"

namespace mycelium {

class FlatbuffersEncoder final : public Encoder {
 public:
  InputOutputDataType OutputType() const override;
  std::vector<ByteBuffer> SerializeFromArrow(const ArrowRecord& rec) const override;
};

}  // namespace mycelium
