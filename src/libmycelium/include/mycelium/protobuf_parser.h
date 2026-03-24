#pragma once
#include <memory>
#include <vector>
#include <google/protobuf/message.h>
#include "mycelium/transformer.h"

namespace mycelium {

class ProtobufParser final : public Parser {
 public:
  explicit ProtobufParser(std::unique_ptr<google::protobuf::Message> template_message);

  InputOutputDataType InputType() const override;
  bool Validate(const ByteBuffer& input_data) const override;
  Result<ParsedRow> Parse(const ByteBuffer& data) const override;

 private:
  std::unique_ptr<google::protobuf::Message> template_;
};

}  // namespace mycelium
