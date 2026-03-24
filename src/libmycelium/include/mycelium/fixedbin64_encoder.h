#pragma once
#include <cstdint>
#include "mycelium/transformer.h"

namespace mycelium {

class FixedBin64Encoder final : public Encoder {
 public:
  InputOutputDataType OutputType() const override;
  std::vector<ByteBuffer> SerializeFromArrow(const ArrowRecord& rec) const override;

 private:
  static void AppendFixed64LE(ByteBuffer* out, std::uint64_t v);
};

}  // namespace mycelium
