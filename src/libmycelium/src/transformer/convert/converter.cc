#include "converter.h"   // → mycelium/converter.h

namespace mycelium {

std::vector<ParsedRow> Converter::Transform(std::string_view /*key*/,
                                            const ParsedRow& input) const {
  // Converter is a pure format-change: pass the ParsedRow through unchanged.
  // The SchemaDescriptor's output encoder re-serializes it in the target format.
  return {input};
}

}  // namespace mycelium
