#include "augmenter.h"   // → mycelium/augmenter.h

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mycelium {

std::vector<ParsedRow> Augmenter::Transform(std::string_view  key,
                                            const ParsedRow&  input) const {
  if (input.empty()) return {};

  // Use the public constants from augmenter.h (single source of truth).
  // kFieldSep = "%%", kIndexKeySep = "$$$KEY$$$"

  std::vector<ParsedRow> outputs;
  outputs.reserve(index_positions_.size());

  for (int32_t idx = 0; idx < static_cast<int32_t>(index_positions_.size()); ++idx) {
    const auto& positions = index_positions_[static_cast<size_t>(idx)];

    // Build: field0%%field1%%...$$$KEY$$$<primary_key>
    std::string prefix;
    prefix.reserve(128);

    for (size_t j = 0; j < positions.size(); ++j) {
      const int pos = positions[j];
      if (pos < 0 || static_cast<size_t>(pos) >= input.size()) return {};
      if (j > 0) prefix.append(kFieldSep);
      const auto& field_val = input.at(static_cast<size_t>(pos)).value;
      if (field_val.kind == FieldValue::Kind::Bytes) {
        prefix.append(field_val.bytes);
      } else {
        prefix.append(field_val.ToString());
      }
    }

    std::string prefix_only = prefix;

    prefix.append(kIndexKeySep);
    prefix.append(key.data(), key.size());

    // Emit a 4-field ParsedRow: { index_no, index_key, primary_key, index_prefix }
    ParsedRow out;
    out.fields.reserve(4);

    ParsedField f_no;
    f_no.name         = "index_no";
    f_no.field_number = 1;
    f_no.value        = FieldValue::MakeInt32(idx);

    ParsedField f_key;
    f_key.name         = "index_key";
    f_key.field_number = 2;
    f_key.value        = FieldValue::MakeBytes(std::move(prefix));

    ParsedField f_pk;
    f_pk.name         = "primary_key";
    f_pk.field_number = 3;
    f_pk.value        = FieldValue::MakeBytes(std::string(key));

    ParsedField f_prefix;
    f_prefix.name         = "index_prefix";
    f_prefix.field_number = 4;
    f_prefix.value        = FieldValue::MakeBytes(std::move(prefix_only));

    out.fields.push_back(std::move(f_no));
    out.fields.push_back(std::move(f_key));
    out.fields.push_back(std::move(f_pk));
    out.fields.push_back(std::move(f_prefix));

    outputs.push_back(std::move(out));
  }

  return outputs;
}

std::vector<ParsedRow> Augmenter::TransformMove(std::string_view key,
                                                ParsedRow&& input) const {
  if (input.empty()) return {};

  std::vector<ParsedRow> outputs;
  outputs.reserve(index_positions_.size());

  for (int32_t idx = 0; idx < static_cast<int32_t>(index_positions_.size()); ++idx) {
    const auto& positions = index_positions_[static_cast<size_t>(idx)];

    // Build: field0%%field1%%...
    std::string prefix;
    if (positions.size() == 1) {
      const int pos = positions[0];
      if (pos < 0 || static_cast<size_t>(pos) >= input.size()) return {};
      auto& field_val = input.at(static_cast<size_t>(pos)).value;
      if (field_val.kind == FieldValue::Kind::Bytes) {
        prefix = std::move(field_val.bytes);
      } else {
        prefix = field_val.ToString();
      }
    } else {
      prefix.reserve(128);
      for (size_t j = 0; j < positions.size(); ++j) {
        const int pos = positions[j];
        if (pos < 0 || static_cast<size_t>(pos) >= input.size()) return {};
        if (j > 0) prefix.append(kFieldSep);
        auto& field_val = input.at(static_cast<size_t>(pos)).value;
        if (field_val.kind == FieldValue::Kind::Bytes) {
          prefix.append(field_val.bytes);
        } else {
          prefix.append(field_val.ToString());
        }
      }
    }

    std::string prefix_only = prefix;

    prefix.append(kIndexKeySep);
    prefix.append(key.data(), key.size());

    ParsedRow out;
    out.fields.reserve(4);

    ParsedField f_no;
    f_no.name         = "index_no";
    f_no.field_number = 1;
    f_no.value        = FieldValue::MakeInt32(idx);

    ParsedField f_key;
    f_key.name         = "index_key";
    f_key.field_number = 2;
    f_key.value        = FieldValue::MakeBytes(std::move(prefix));

    ParsedField f_pk;
    f_pk.name         = "primary_key";
    f_pk.field_number = 3;
    f_pk.value        = FieldValue::MakeBytes(std::string(key));

    ParsedField f_prefix;
    f_prefix.name         = "index_prefix";
    f_prefix.field_number = 4;
    f_prefix.value        = FieldValue::MakeBytes(std::move(prefix_only));

    out.fields.push_back(std::move(f_no));
    out.fields.push_back(std::move(f_key));
    out.fields.push_back(std::move(f_pk));
    out.fields.push_back(std::move(f_prefix));

    outputs.push_back(std::move(out));
  }

  return outputs;
}

}  // namespace mycelium
