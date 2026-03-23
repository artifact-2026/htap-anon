#include <memory>
#include "flatbuffers/flatbuffers.h"
#include "mycelium/transformer.h"
#include "row_generated.h"
#include "data.pb.h"

namespace mycelium {

class Converter final : public Transformer {
 public:
  std::string Name() const override { return "convert_transformer"; }
  TransformerType Supports() const override { return TransformerType::CONVERTER; }

  std::vector<ArrowRecord> Transform(
    std::string_view key,
    const ArrowRecord& input) const override;
};

}