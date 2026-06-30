#include "mynooper.h"   // -> mycelium/mynooper.h

namespace mycelium {

std::vector<ParsedRow> Mynooper::Transform(std::string_view /*key*/,
                                           const ParsedRow& input) const {
  return {input};
}

std::vector<ParsedRow> Mynooper::TransformMove(std::string_view /*key*/,
                                                ParsedRow&& input) const {
  std::vector<ParsedRow> out;
  out.push_back(std::move(input));
  return out;
}

}  // namespace mycelium
