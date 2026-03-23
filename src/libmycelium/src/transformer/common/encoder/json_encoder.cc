#include "json_encoder.h"

#include <cstdint>
#include <memory>
#include <string>

#include <arrow/io/memory.h>
#include <arrow/scalar.h>
#include <arrow/type.h>
#include <arrow/util/checked_cast.h>
#include <arrow/result.h>
#include <arrow/status.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <jsoncpp/json/json.h>

namespace mycelium {

InputOutputDataType JsonEncoder::OutputType() const {
  return InputOutputDataType::JSON;
} // namespace mycelium

namespace {
static inline std::vector<ByteBuffer> SplitLinesToByteBuffers(const std::string& s) {
  std::vector<ByteBuffer> lines;
  size_t start = 0;

  while (start < s.size()) {
    size_t end = s.find('\n', start);
    if (end == std::string::npos) end = s.size();

    if (end > start) {
      const uint8_t* p = reinterpret_cast<const uint8_t*>(s.data() + start);
      lines.emplace_back(p, p + (end - start));  // copy bytes of the line
    }

    start = end + 1;
  }

  return lines;
} // namespace mycelium

static inline std::string BytesToString(const uint8_t* p, int64_t n) {
  return std::string(reinterpret_cast<const char*>(p),
                     reinterpret_cast<const char*>(p) + n);
} // namespace mycelium

} // namespace mycelium

std::vector<ByteBuffer> JsonEncoder::SerializeFromArrow(const ArrowRecord& rb) const {
  if (!rb) return {};
  if (!rb->schema()) return {};

  const int64_t rows = rb->num_rows();
  const int cols = rb->num_columns();

  std::vector<ByteBuffer> out;
  out.reserve(static_cast<size_t>(rows));

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";          // compact
  wb["emitUTF8"] = true;

  for (int64_t r = 0; r < rows; ++r) {
    Json::Value obj(Json::objectValue);

    for (int c = 0; c < cols; ++c) {
      const std::string key = rb->schema()->field(c)->name();

      const auto& arr = rb->column(c);
      if (arr->IsNull(r)) {
        continue;
      }

      if (arr->type_id() == arrow::Type::BINARY) {
        auto a = std::static_pointer_cast<arrow::BinaryArray>(arr);
        int32_t n = 0;
        const uint8_t* p = a->GetValue(static_cast<int64_t>(r), &n);
        obj[key] = BytesToString(p, n);
      } else { // LARGE_BINARY
        auto a = std::static_pointer_cast<arrow::LargeBinaryArray>(arr);
        int64_t n = 0;
        const uint8_t* p = a->GetValue(static_cast<int64_t>(r), &n);
        obj[key] = BytesToString(p, n);
      }
    }

    std::unique_ptr<Json::StreamWriter> w(wb.newStreamWriter());
    std::ostringstream oss;
    w->write(obj, &oss);
    std::string s = oss.str();

    ByteBuffer buf;
    buf.resize(s.size());
    std::memcpy(buf.data(), s.data(), s.size());
    out.push_back(std::move(buf));
  }

  return out; 
} // namespace mycelium

}  // namespace mycelium
