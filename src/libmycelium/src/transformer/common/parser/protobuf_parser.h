#pragma once

#include "mycelium/transformer.h"

#include <google/protobuf/message.h>
#include <memory>
#include <vector>

namespace mycelium {
// A Parser that parses PROTOBUF bytes into a google::protobuf::Message.
// It uses one "template" message per output type (or per schema) to New() instances.
class ProtobufParser final : public Parser {
 public:
  // The caller owns template_ and must keep it alive for the parser lifetime,
  // or pass ownership by using unique_ptr and storing it internally.
  explicit ProtobufParser(std::unique_ptr<google::protobuf::Message> template_message);

  InputOutputDataType InputType() const override;
  bool Validate(const ByteBuffer& input_data) const override;
  arrow::Result<ArrowRecord> ParseToArrow(const ByteBuffer& data) const override;

 private:
  std::unique_ptr<google::protobuf::Message> template_;
};

}