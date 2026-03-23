#include <iostream>
#include "mynooper.h"

namespace mycelium {

std::vector<ArrowRecord> Mynooper::Transform(
      std::string_view key,
      const ArrowRecord& input) const
{
    (void)key;
    return {input};
} // namespace mycelium

}