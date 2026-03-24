#pragma once
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "mycelium/transformer.h"
#include "flatbuffers/reflection_generated.h"
#include "flatbuffers/util.h"

namespace mycelium {

class FlatbuffersParser final : public Parser {
 public:
  explicit FlatbuffersParser(const std::string& bfbs_path) {
    std::string contents;
    if (!flatbuffers::LoadFile(bfbs_path.c_str(), /*binary=*/true, &contents)) {
      throw std::runtime_error(
          "FlatbuffersParser: failed to load bfbs file: " + bfbs_path);
    }
    bfbs_bytes_ = std::move(contents);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(bfbs_bytes_.data());
    schema_ = std::shared_ptr<const reflection::Schema>(
        reflection::GetSchema(p),
        [](const reflection::Schema*) { /* no-op: owned by bfbs_bytes_ */ });
    root_obj_ = schema_->root_table();
    if (!root_obj_) {
      throw std::runtime_error("FlatbuffersParser: bfbs schema has no root_table()");
    }
  }

  FlatbuffersParser(const std::string& bfbs_path,
                    const std::string& root_object_name)
      : FlatbuffersParser(bfbs_path) {
    const reflection::Object* found = nullptr;
    if (schema_->objects()) {
      for (auto it = schema_->objects()->begin();
           it != schema_->objects()->end(); ++it) {
        const reflection::Object* obj = *it;
        if (obj && obj->name() && obj->name()->str() == root_object_name) {
          found = obj;
          break;
        }
      }
    }
    if (!found) {
      throw std::runtime_error(
          "FlatbuffersParser: root object not found in schema: " + root_object_name);
    }
    root_obj_ = found;
  }

  InputOutputDataType InputType() const override { return InputOutputDataType::FLATBUFFERS; }
  bool Validate(const ByteBuffer& input_data) const override;
  Result<ParsedRow> Parse(const ByteBuffer& data) const override;

 private:
  std::string bfbs_bytes_;
  std::shared_ptr<const reflection::Schema> schema_;
  const reflection::Object* root_obj_ = nullptr;
};

}  // namespace mycelium
