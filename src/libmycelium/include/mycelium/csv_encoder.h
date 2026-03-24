#pragma once
#include <string>
#include "mycelium/transformer.h"

namespace mycelium {

class CsvEncoder final : public Encoder {
 public:
  InputOutputDataType OutputType() const override;
  std::vector<ByteBuffer> Serialize(const ParsedRow& row) const override;

 private:
  static void AppendField(std::string* out, const std::string& f);
};

}  // namespace mycelium
