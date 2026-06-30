#include "mycelium/transformer.h"
#include "distribute/distributor.h"
#include "convert/converter.h"
#include "augment/augmenter.h"
#include "identity/mynooper.h"

namespace mycelium {
std::shared_ptr<Transformer> CreateTransformer(const TransformerType transformer_type) {
    std::vector<std::vector<int>> splits, indpos;
    switch (transformer_type) {
        case TransformerType::DISTRIBUTOR:
            return std::make_shared<Distributor>(splits);
        case TransformerType::CONVERTER:
            return std::make_shared<Converter>();
        case TransformerType::AUGMENTER:
            return std::make_shared<Augmenter>(indpos);
        case TransformerType::MYNOOPER:
            return std::make_shared<Mynooper>();
        case TransformerType::NOTRANSFORMATION:
        default:
            return nullptr;  // Handle invalid type
    }
}
}  // namespace mycelium