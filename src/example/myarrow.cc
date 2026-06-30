#include <arrow/api.h>
#include <iostream>
#include <memory>
#include <vector>

// Dummy struct just to match your signature
struct AdditionalInfo {
  int dummy = 0;
};

using ArrowArrayPtr = std::shared_ptr<arrow::Array>;
using ArrowArrayVec = std::vector<ArrowArrayPtr>;

// Your function signature:
arrow::Result<ArrowArrayVec> Transform(
    const ArrowArrayPtr& input,
    AdditionalInfo& info) {

  // For this simple demo, we expect a string array as input.
  if (input->type_id() != arrow::Type::STRING) {
    return arrow::Status::Invalid("Transform expects a STRING array as input");
  }

  auto str_array = std::static_pointer_cast<arrow::StringArray>(input);

  ArrowArrayVec outputs;
  outputs.reserve(str_array->length());

  // We'll reuse a StringBuilder to create 1-element arrays.
  arrow::StringBuilder builder;

  for (int64_t i = 0; i < str_array->length(); ++i) {
    if (str_array->IsNull(i)) {
      // For simplicity, skip nulls (you could also emit a null array).
      continue;
    }

    // Reset builder to build a fresh 1-element array.
    builder.Reset();

    // Append a single value from the input.
    ARROW_RETURN_NOT_OK(builder.Append(str_array->GetString(i)));

    std::shared_ptr<arrow::Array> one_elem_array;
    ARROW_RETURN_NOT_OK(builder.Finish(&one_elem_array));

    outputs.push_back(one_elem_array);
  }

  return outputs;
}

int main() {
  // 1) Build the input Arrow array: ["abc", "345", "tree", "123", "food"]
  arrow::StringBuilder builder;
  builder.Append("abc");
  builder.Append("345");
  builder.Append("tree");
  builder.Append("123");
  builder.Append("food");

  std::shared_ptr<arrow::Array> input;
  auto status = builder.Finish(&input);
  if (!status.ok()) {
    std::cerr << "Failed to build input array: " << status.ToString() << "\n";
    return 1;
  }

  std::cout << "Input array: " << input->ToString() << "\n";

  // 2) Call Transform
  AdditionalInfo info;
  auto result = Transform(input, info);
  if (!result.ok()) {
    std::cerr << "Transform failed: " << result.status().ToString() << "\n";
    return 1;
  }

  ArrowArrayVec outputs = *result;

  // 3) Print outputs
  std::cout << "Number of output arrays: " << outputs.size() << "\n";
  for (size_t i = 0; i < outputs.size(); ++i) {
    auto out_arr = std::static_pointer_cast<arrow::StringArray>(outputs[i]);
    std::cout << "output[" << i << "] (len=" << out_arr->length() << "): ";

    for (int64_t j = 0; j < out_arr->length(); ++j) {
      if (out_arr->IsValid(j)) {
        std::cout << out_arr->GetString(j);
      } else {
        std::cout << "<null>";
      }
      if (j + 1 < out_arr->length()) {
        std::cout << ", ";
      }
    }
    std::cout << "\n";
  }

  return 0;
}
