#include "distributor.h"  

#include <unordered_set>
#include <utility>
#include <vector>

#include <arrow/result.h>
#include <arrow/scalar.h>
#include <arrow/status.h>
#include <arrow/type.h>
#include <arrow/type_traits.h>
#include <arrow/util/checked_cast.h>

namespace mycelium {

namespace {

bool ValidateSplitGroup(const std::vector<int>& cols, int32_t num_fields) {
  if (cols.empty()) return false;

  std::unordered_set<int> seen;
  seen.reserve(cols.size());

  for (int c : cols) {
    if (c < 0) return false;
    if (c >= num_fields) return false;
    if (!seen.insert(c).second) return false;  // duplicate within the group
  }
  return true;
} // namespace mycelium

}  // namespace

std::vector<ArrowRecord> Distributor::Transform(std::string_view /*key*/,
                                               const ArrowRecord& input) const {
  std::vector<ArrowRecord> outputs;
  outputs.reserve(splits_.size());

  if (!input) {
    // No input => no outputs (policy choice).
    return outputs;
  }

  const auto& schema = input->schema();
  if (!schema) {
    return {};
  }

  const int32_t num_cols = input->num_columns();
  const int64_t num_rows = input->num_rows();



  // For each requested split group, build a new Schema + RecordBatch.
  for (const auto& cols : splits_) {
    if (!ValidateSplitGroup(cols, num_cols)) {
      // With this API (no Status return), fail-fast with empty result.
      return {};
    }

    std::vector<std::shared_ptr<arrow::Field>> out_fields;
    out_fields.reserve(cols.size());

    std::vector<std::shared_ptr<arrow::Array>> out_arrays;
    out_arrays.reserve(cols.size());

    for (int idx : cols) {
      // Schema/field for this column.
      const std::shared_ptr<arrow::Field>& f = schema->field(idx);
      if (!f) {
        return {};
      }
      out_fields.emplace_back(f);

      // Column data for this column.
      std::shared_ptr<arrow::Array> arr = input->column(idx);
      if (!arr) {
        return {};
      }

      if (arr->length() != num_rows) {
        return {};
      }
      out_arrays.emplace_back(std::move(arr));
    }

    auto out_schema = arrow::schema(std::move(out_fields));
    outputs.emplace_back(arrow::RecordBatch::Make(out_schema, num_rows, std::move(out_arrays)));
  }
  
  return outputs;
} // namespace mycelium

}  // namespace mycelium