// proto2flat_from_hex.cc
// Usage:
//   sst_dump --command=scan --file=".../000011.sst" --output_hex 
//     | awk -F'=> ' '/=>/ && NF==2 {print $2}' 
//     | ./proto2flat_from_hex /path/to/output_rows.fb

#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "data.pb.h"                 // generated from your updated data.proto
#include "row_generated.h"      // generated via: flatc -c row.fbs
#include "flatbuffers/flatbuffers.h"

// --- Helpers ----------------------------------------------------------------

static bool HexToBytes(std::string_view hex, std::string &out) {
  auto is_hex = [](unsigned char c) {
    return std::isxdigit(c);
  };
  // trim whitespace
  size_t i = 0, j = hex.size();
  while (i < j && std::isspace((unsigned char)hex[i])) ++i;
  while (j > i && std::isspace((unsigned char)hex[j - 1])) --j;
  if (i >= j) { out.clear(); return true; }
  // ignore optional 0x prefix
  if (j - i >= 2 && hex[i] == '0' && (hex[i+1] == 'x' || hex[i+1] == 'X')) i += 2;

  size_t n = j - i;
  if (n % 2 != 0) return false;
  out.resize(n / 2);
  auto val = [](unsigned char c)->int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
  };
  for (size_t k = 0; k < out.size(); ++k) {
    unsigned char c1 = hex[i + 2*k], c2 = hex[i + 2*k + 1];
    if (!is_hex(c1) || !is_hex(c2)) return false;
    int hi = val(c1), lo = val(c2);
    if (hi < 0 || lo < 0) return false;
    out[k] = static_cast<char>((hi << 4) | lo);
  }
  return true;
}

// Write a little-endian 32-bit length prefix followed by the bytes.
static bool WriteFrame(std::ofstream &ofs, const uint8_t *buf, size_t sz) {
  uint32_t len = static_cast<uint32_t>(sz);
  char hdr[4];
  hdr[0] = static_cast<char>(len & 0xFF);
  hdr[1] = static_cast<char>((len >> 8) & 0xFF);
  hdr[2] = static_cast<char>((len >> 16) & 0xFF);
  hdr[3] = static_cast<char>((len >> 24) & 0xFF);
  ofs.write(hdr, 4);
  if (!ofs) return false;
  ofs.write(reinterpret_cast<const char*>(buf), static_cast<std::streamsize>(sz));
  return !!ofs;
}

// Convert one Protobuf Row -> FlatBuffers Row into `fbb`.
static flatbuffers::Offset<flat::Row>
BuildFbRow(flatbuffers::FlatBufferBuilder &fbb, const data::Row &pr) {
  std::vector<flatbuffers::Offset<flat::Column>> cols;
  cols.reserve(pr.columns_size());
  for (const auto &c : pr.columns()) {
    auto name  = fbb.CreateString(c.name());
    auto value = fbb.CreateVector(
        reinterpret_cast<const uint8_t*>(c.value().data()),
        static_cast<size_t>(c.value().size()));
    cols.push_back(flat::CreateColumn(fbb, name, value));
  }
  auto cols_vec = fbb.CreateVector(cols);
  return flat::CreateRow(fbb, cols_vec);
}

// --- Main -------------------------------------------------------------------

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " /path/to/output_rows.fb\n";
    return 2;
  }
  const std::string out_path = argv[1];

  std::ofstream ofs(out_path, std::ios::binary);
  if (!ofs) {
    std::cerr << "Error: cannot open output file: " << out_path << "\n";
    return 2;
  }

  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);

  std::string line;
  std::string pb_bytes;
  size_t count = 0, ok = 0, fail = 0;

  while (std::getline(std::cin, line)) {
    ++count;
    // Skip empty/noise lines
    bool only_ws = true;
    for (char ch : line) { if (!std::isspace((unsigned char)ch)) { only_ws = false; break; } }
    if (only_ws) continue;

    if (!HexToBytes(line, pb_bytes)) {
      std::cerr << "[warn] line " << count << ": bad hex, skipping\n";
      ++fail; continue;
    }

    data::Row pr;
    if (!pr.ParseFromString(pb_bytes)) {
      std::cerr << "[warn] line " << count << ": Protobuf parse failed, skipping\n";
      ++fail; continue;
    }

    flatbuffers::FlatBufferBuilder fbb(1024);
    auto row_off = BuildFbRow(fbb, pr);
    fbb.Finish(row_off); // root_type Row

    const uint8_t *buf = fbb.GetBufferPointer();
    size_t sz = fbb.GetSize();
    if (!WriteFrame(ofs, buf, sz)) {
      std::cerr << "Error: write failed at record " << count << "\n";
      return 3;
    }
    ++ok;
  }

  ofs.flush();
  if (!ofs) {
    std::cerr << "Error: final flush failed\n";
    return 3;
  }

  std::cerr << "Done. Converted " << ok << " rows; skipped " << fail << ".\n";
  return 0;
}