#include "json_encoder.h"   // → mycelium/json_encoder.h

#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <jsoncpp/json/json.h>

namespace mycelium {

InputOutputDataType JsonEncoder::OutputType() const {
  return InputOutputDataType::JSON;
}

std::vector<ByteBuffer> JsonEncoder::Serialize(const ParsedRow& row) const {
  if (row.empty()) return {};

  Json::Value obj(Json::objectValue);

  for (const auto& pf : row.fields) {
    if (pf.value.is_null()) continue;
    // Store all values as their string representation.
    // Binary/string fields are written verbatim; numerics use ToString().
    obj[pf.name] = pf.value.ToString();
  }

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";   // compact
  wb["emitUTF8"]    = true;

  std::unique_ptr<Json::StreamWriter> w(wb.newStreamWriter());
  std::ostringstream oss;
  w->write(obj, &oss);
  const std::string s = oss.str();

  ByteBuffer buf(s.begin(), s.end());
  return {std::move(buf)};
}

}  // namespace mycelium
