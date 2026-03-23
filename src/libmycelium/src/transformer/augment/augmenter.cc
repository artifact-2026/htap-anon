#include <arrow/buffer.h>
#include <arrow/result.h>
#include <arrow/status.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "augmenter.h"

namespace mycelium {

std::vector<ArrowRecord> Augmenter::Transform(
    std::string_view key,
    const ArrowRecord& input) const {
  std::vector<ArrowRecord> outputs;

  if (!input) return outputs;

  const int32_t num_cols = input->num_columns();
  const int64_t num_rows = input->num_rows();
  if (num_rows != 1) return outputs;

  constexpr std::string_view key_field_separator = "%%";
  constexpr std::string_view original_key_separator = "$$$KEY$$$";

  // key is already string_view — use directly.
  const std::string_view pk_sv = key;

  auto out_schema = arrow::schema({
      arrow::field("index_no", arrow::int32(),  /*nullable=*/false),
      arrow::field("index_key", arrow::binary(), /*nullable=*/false),
      arrow::field("primary_key", arrow::binary(), /*nullable=*/false),
  });

  outputs.reserve(index_positions_.size());

  auto AppendCellBytes = [&](const std::shared_ptr<arrow::Array>& arr,
                             int64_t row,
                             std::string* out) -> bool {
    if (!arr) return false;
    if (row < 0 || row >= arr->length()) return false;
    if (arr->IsNull(row)) return false;

    switch (arr->type_id()) {
      case arrow::Type::STRING: {
        const auto& a = arrow::internal::checked_cast<const arrow::StringArray&>(*arr);
        auto v = a.GetView(row);               // view into array buffer
        out->append(v.data(), v.size());
        return true;
      }
      case arrow::Type::BINARY: {
        const auto& a = arrow::internal::checked_cast<const arrow::BinaryArray&>(*arr);
        auto v = a.GetView(row);
        out->append(v.data(), v.size());
        return true;
      }
      case arrow::Type::LARGE_STRING: {
        const auto& a = arrow::internal::checked_cast<const arrow::LargeStringArray&>(*arr);
        auto v = a.GetView(row);
        out->append(v.data(), v.size());
        return true;
      }
      case arrow::Type::LARGE_BINARY: {
        const auto& a = arrow::internal::checked_cast<const arrow::LargeBinaryArray&>(*arr);
        auto v = a.GetView(row);
        out->append(v.data(), v.size());
        return true;
      }
      default: {
        auto maybe_scalar = arr->GetScalar(row);
        if (!maybe_scalar.ok()) return false;
        const auto& s = *maybe_scalar;
        if (!s || !s->is_valid) return false;
        const std::string str = s->ToString();
        out->append(str.data(), str.size());
        return true;
      }
    }
  };

  for (int32_t idx = 0; idx < static_cast<int32_t>(index_positions_.size()); ++idx) {
    const auto& positions = index_positions_[static_cast<size_t>(idx)];

    // Build prefixIndexKey bytes = f0%%f1%%...$$$KEY$$$<primary_key>
    std::string prefix;
    prefix.reserve(128);  // heuristic

    for (size_t j = 0; j < positions.size(); ++j) {
      const int pos = positions[j];
      if (pos < 0 || pos >= num_cols) return {};

      if (j > 0) prefix.append(key_field_separator);

      // Convert field scalar to bytes.
      const auto& arr = input->column(pos);
      if (!AppendCellBytes(arr, /*row=*/0, &prefix)) return {};
    }

    prefix.append(original_key_separator);
    prefix.append(pk_sv.data(), pk_sv.size());

    if (pk_sv.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) return {};

    // Build arrays for a 1-row batch.
    arrow::Int32Builder index_no_b;
    arrow::BinaryBuilder index_key_b;
    arrow::BinaryBuilder pk_b;

    if (!index_no_b.Append(idx).ok()) return {};
    if (!index_key_b.Append(prefix).ok()) return {};
    if (!pk_b.Append(reinterpret_cast<const uint8_t*>(pk_sv.data()),
                     static_cast<int32_t>(pk_sv.size())).ok()) return {};

    std::shared_ptr<arrow::Array> index_no_arr, index_key_arr, pk_arr;
    if (!index_no_b.Finish(&index_no_arr).ok()) return {};
    if (!index_key_b.Finish(&index_key_arr).ok()) return {};
    if (!pk_b.Finish(&pk_arr).ok()) return {};

    outputs.push_back(arrow::RecordBatch::Make(
        out_schema, /*num_rows=*/1,
        {index_no_arr, index_key_arr, pk_arr}));
  }

  return outputs;
} // namespace mycelium

}  // namespace mycelium
