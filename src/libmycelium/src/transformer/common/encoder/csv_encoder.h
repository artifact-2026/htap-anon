#pragma once

#include "mycelium/transformer.h"
#include "../parser/csv_parser.h"  // CsvRowPayload

namespace mycelium {

class CsvEncoder final : public Encoder {
 public:
  InputOutputDataType OutputType() const override;
  std::vector<ByteBuffer> SerializeFromArrow(const ArrowRecord& rec) const override;

 private:
  static void AppendField(std::string* out, const std::string& f);
  std::string ScalarToStringForCsv(const arrow::Scalar& s) const;
};

}