#include "mynooper.h"   // -> mycelium/mynooper.h

namespace mycelium {

std::vector<ParsedRow> Mynooper::Transform(std::string_view /*key*/,
                                           const ParsedRow& input) const {
  return {input};
}

}  // namespace mycelium
