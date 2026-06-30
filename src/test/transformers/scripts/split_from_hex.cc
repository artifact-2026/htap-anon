// split_from_hex.cc
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <cctype>

#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/util/delimited_message_util.h>
#include "data.pb.h"

namespace io = google::protobuf::io;

static bool HexToBytes(const std::string& hex, std::string& out) {
  out.clear();
  out.reserve(hex.size()/2);
  auto hexval = [](char c) -> int {
    if ('0'<=c && c<='9') return c-'0';
    if ('a'<=c && c<='f') return c-'a'+10;
    if ('A'<=c && c<='F') return c-'A'+10;
    return -1;
  };
  size_t i = 0;
  while (i < hex.size()) {
    while (i < hex.size() && std::isspace(static_cast<unsigned char>(hex[i]))) ++i;
    if (i >= hex.size()) break;
    int hi = hexval(hex[i++]); if (hi < 0) return false;
    if (i >= hex.size()) return false;
    int lo = hexval(hex[i++]); if (lo < 0) return false;
    out.push_back(static_cast<char>((hi<<4) | lo));
  }
  return true;
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: " << argv[0] << " OUT_A.binpb OUT_B.binpb\n";
    return 1;
  }
  std::ofstream fa(argv[1], std::ios::binary), fb(argv[2], std::ios::binary);
  if (!fa || !fb) { std::cerr << "Failed to open outputs\n"; return 2; }

  io::OstreamOutputStream za(&fa), zb(&fb);

  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);

  std::string line, bytes;
  uint64_t in_lines = 0, parsed = 0, written = 0;

  data::Row row, rowA, rowB;
  while (std::getline(std::cin, line)) {
    ++in_lines;
    if (!HexToBytes(line, bytes)) continue;

    row.Clear();
    if (!row.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
      continue; // skip non-Row values
    }
    ++parsed;

    // split
    rowA.Clear(); rowB.Clear(); 
    const int n = row.columns_size();
    const int half = n/2;
    rowA.mutable_columns()->Reserve(half);
    rowB.mutable_columns()->Reserve(n-half);
    for (int i = 0; i < n; ++i) {
      const auto& src = row.columns(i);
      auto* dst = (i < half) ? rowA.add_columns() : rowB.add_columns();
      dst->set_name(src.name());
      dst->set_value(src.value());
    }

    if (!google::protobuf::util::SerializeDelimitedToZeroCopyStream(rowA, &za)) { std::cerr << "write A failed\n"; return 3; }
    if (!google::protobuf::util::SerializeDelimitedToZeroCopyStream(rowB, &zb)) { std::cerr << "write B failed\n"; return 4; }
    ++written;
  }

  std::cerr << "Done. lines=" << in_lines << " parsed_rows=" << parsed << " written_pairs=" << written << "\n";
  return 0;
}