#include "transformer.h"

namespace ROCKSDB_NAMESPACE {

class Cracker : public Transformer
{
  public:
    Cracker() {};
  private:
    void do_transformation(std::string input, std::vector<std::string> outputs, int splits) override;
};

}