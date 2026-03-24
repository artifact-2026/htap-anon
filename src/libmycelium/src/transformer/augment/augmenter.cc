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

  static constexpr std::string_view kFieldSep = "%%";
  static constexpr std::string_view kKeySep   = "$$$KEY$$$";

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
      prefix.append(input.at(static_cast<size_t>(pos)).value.ToString());
    }

    prefix.append(kKeySep);
    prefix.append(key.data(), key.size());

    // Emit a 3-field ParsedRow: { index_no, index_key, primary_key }
    ParsedRow out;
    out.fields.reserve(3);

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

    out.fields.push_back(std::move(f_no));
    out.fields.push_back(std::move(f_key));
    out.fields.push_back(std::move(f_pk));

    outputs.push_back(std::move(out));
  }

  return outputs;
}

}  // namespace mycelium
