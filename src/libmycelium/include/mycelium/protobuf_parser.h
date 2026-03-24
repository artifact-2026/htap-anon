#pragma once
// google::protobuf::Message appears in the constructor signature and as a
// member, so the protobuf header must be included here.
#include <memory>
#include <vector>
#include <google/protobuf/message.h>
#include "mycelium/transformer.h"

namespace mycelium {

// Parses PROTOBUF bytes into an ArrowRecord via a template message instance.
class ProtobufParser final : public Parser {
 public:
  explicit ProtobufParser(
      std::unique_ptr<google::protobuf::Message> template_message);

  InputOutputDataType InputType() const override;
  bool Validate(const ByteBuffer& input_data) const override;
  arrow::Result<ArrowRecord> ParseToArrow(const ByteBuffer& data) const override;

 private:
  std::unique_ptr<google::protobuf::Message> template_;
};

}  // namespace mycelium
