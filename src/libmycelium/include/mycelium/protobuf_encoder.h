#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "mycelium/transformer.h"

namespace mycelium {

// Encodes an ArrowRecord as a raw protobuf BytesRow message
//   message BytesRow { repeated bytes col = 1; }
// No protobuf library dependency in this header.
class ProtobufBytesRowEncoder final : public Encoder {
 public:
  explicit ProtobufBytesRowEncoder(size_t num_cols) : num_cols_(num_cols) {}

  InputOutputDataType OutputType() const override {
    return InputOutputDataType::PROTOBUF;
  }

  std::vector<ByteBuffer> SerializeFromArrow(const ArrowRecord& rec) const override;

 private:
  size_t num_cols_;
};

}  // namespace mycelium
