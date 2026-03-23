#pragma once

#include <memory>
#include "mycelium/transformer.h"

namespace mycelium {

class Mynooper : public Transformer {
public:
    Mynooper() {};
    ~Mynooper() {};

    std::string Name() const override { return "Mycelium-NoOp"; }

    std::vector<ArrowRecord> Transform(
      std::string_view key,
      const ArrowRecord& input) const override;
    
    TransformerType Supports() const override { return TransformerType::MYNOOPER; }
};

} // namespace mycelium