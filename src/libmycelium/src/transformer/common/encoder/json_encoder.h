#pragma once

#include "mycelium/transformer.h"

namespace mycelium {

class JsonEncoder final : public Encoder {
 public:
  InputOutputDataType OutputType() const override;
  std::vector<ByteBuffer> SerializeFromArrow(const ArrowRecord& rec) const override;
};

}