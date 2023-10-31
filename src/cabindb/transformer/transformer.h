#include <string>
#include <vector>

namespace ROCKSDB_NAMESPACE {

class Transformer
{
  public:
    // non-virtual interface
    void transform(std::string input, std::vector<std::string> outputs, int splits) {
      do_transformation(input, outputs, splits); } // equivalent to "this->do_transformation()"

    // enable deletion of a Derived* through a Base*
    virtual ~Transformer() = default;    
  private:
    // pure virtual implementation
    virtual void do_transformation(std::string input, std::vector<std::string> outputs, int splits) = 0;
};

}
