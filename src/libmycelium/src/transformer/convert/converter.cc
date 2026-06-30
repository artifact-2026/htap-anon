#include "converter.h"   // → mycelium/converter.h

namespace mycelium {

std::vector<ParsedRow> Converter::Transform(std::string_view /*key*/,
                                            const ParsedRow& input) const {
  // Converter is a pure format-change: pass the ParsedRow through unchanged.
  // The SchemaDescriptor's output encoder re-serializes it in the target format.
  return {input};
}

std::vector<ParsedRow> Converter::TransformMove(std::string_view /*key*/,
                                                ParsedRow&& input) const {
  std::vector<ParsedRow> out;
  out.push_back(std::move(input));
  return out;
}

}  // namespace mycelium
