#include <memory>
#include <mutex>
#include <functional>
#include "mycelium/transformer.h"
#include "data.pb.h"

namespace mycelium {

class Augmenter : public Transformer
{
  public:
    Augmenter(std::vector<std::vector<int>> ipositions) 
        : index_positions_(ipositions) {};
    ~Augmenter() {};

    std::string Name() const override { return "Augmenter"; }

    std::vector<ArrowRecord> Transform(
      std::string_view key,
      const ArrowRecord& input) const override;
  
    TransformerType Supports() const override { return TransformerType::AUGMENTER; }

    const std::vector<std::vector<int>> GetPositionedIndexKeys() const { return index_positions_; }
  private:
    std::vector<std::vector<int>> index_positions_;
};

}