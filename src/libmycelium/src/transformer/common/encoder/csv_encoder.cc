#include "csv_encoder.h"   // → mycelium/csv_encoder.h

#include <string>
#include <vector>

namespace mycelium {

InputOutputDataType CsvEncoder::OutputType() const {
  return InputOutputDataType::CSV;
}

// static
void CsvEncoder::AppendField(std::string* out, const std::string& f) {
  bool needs_quotes = false;
  for (char c : f) {
    if (c == '"' || c == ',' || c == '\n' || c == '\r') {
      needs_quotes = true;
      break;
    }
  }
  if (!needs_quotes) { out->append(f); return; }

  out->push_back('"');
  for (char c : f) {
    if (c == '"') { out->push_back('"'); out->push_back('"'); }
    else          { out->push_back(c); }
  }
  out->push_back('"');
}

std::vector<ByteBuffer> CsvEncoder::Serialize(const ParsedRow& row) const {
  if (row.empty()) return {};

  std::string line;
  line.reserve(row.size() * 10 + 1);

  for (size_t i = 0; i < row.size(); ++i) {
    if (i) line.push_back(',');
    if (!row.at(i).value.is_null()) {
      AppendField(&line, row.at(i).value.ToString());
    }
  }
  line.push_back('\n');

  return {ByteBuffer(line.begin(), line.end())};
}

}  // namespace mycelium
