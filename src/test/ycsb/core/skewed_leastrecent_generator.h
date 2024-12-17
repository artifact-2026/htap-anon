#ifndef YCSB_C_SKEWED_LEASTRECENT_GENERATOR_H_
#define YCSB_C_SKEWED_LEASTRECENT_GENERATOR_H_

#include "generator.h"

#include <atomic>
#include <cstdint>
#include "counter_generator.h"
#include "zipfian_generator.h"

namespace ycsbc {

class SkewedLeastRecentGenerator : public Generator<uint64_t> {
 public:
  SkewedLeastRecentGenerator(CounterGenerator &counter) :
      basis_(counter), zipfian_(basis_.Last()) {
    Next();
  }
  
  uint64_t Next();
  uint64_t Last() { return last_; }
 private:
  CounterGenerator &basis_;
  ZipfianGenerator zipfian_;
  std::atomic<uint64_t> last_;
};

inline uint64_t SkewedLeastRecentGenerator::Next() {
  uint64_t max = basis_.Last()/4;
  return last_ = max - zipfian_.Next(max) % max;
}

} // ycsbc

#endif // YCSB_C_SKEWED_LEASTRECENT_GENERATOR_H_
