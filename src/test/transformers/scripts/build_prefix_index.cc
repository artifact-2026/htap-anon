// build_prefix_index.cc
// Read `sst_dump --output_hex` lines from stdin, build prefix index on "field1".
// Default: write index keys (one per line) to a text file.
// Optional: --build-sst=OUT.sst to write a RocksDB SST with empty values.
//
// Usage examples:
//
//   # simple text index
//   sst_dump --command=scan --file="/path/to/000011.sst" --output_hex |
//     awk -F'=> ' '/type:1/ && NF==2 {print $0}' |
//     ./build_prefix_index --out index.txt
//
//   # build a RocksDB SST (needs RocksDB installed and linked)
//   sst_dump --command=scan --file="/path/to/000011.sst" --output_hex |
//     awk -F'=> ' '/type:1/ && NF==2 {print $0}' |
//     ./build_prefix_index --build-sst index.sst
//
// Notes:
// - We look for the first single-quoted token in the line as key-hex,
//   and everything after "=>" as value-hex, matching your sst_dump format:
//   '6B6579737472...' seq:..., type:1 => 0A... (value hex)

#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "data.pb.h"  // generated from your columns-only data.proto
// For Mode B (SST output), include RocksDB headers if you want SST output.
// #include <rocksdb/sst_file_writer.h>
// #include <rocksdb/env.h>
// #include <rocksdb/options.h>

namespace {

// ---------- small helpers ----------
bool HexToBytes(std::string_view hex, std::string &out) {
  auto is_hex = [](unsigned char c){ return std::isxdigit(c); };
  // trim
  size_t i=0, j=hex.size();
  while (i<j && std::isspace((unsigned char)hex[i])) ++i;
  while (j>i && std::isspace((unsigned char)hex[j-1])) --j;
  if (i>=j) { out.clear(); return true; }
  // allow quotes around hex (we'll have stripped those already generally)
  // allow 0x prefix
  if (j - i >= 2 && hex[i]=='0' && (hex[i+1]=='x' || hex[i+1]=='X')) i += 2;
  size_t n = j - i;
  if (n % 2) return false;
  out.resize(n/2);
  auto val = [](unsigned char c)->int{
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return 10+(c-'a');
    if (c>='A'&&c<='F') return 10+(c-'A');
    return -1;
  };
  for (size_t k=0; k<out.size(); ++k) {
    unsigned char c1 = hex[i+2*k], c2 = hex[i+2*k+1];
    if (!is_hex(c1) || !is_hex(c2)) return false;
    int hi = val(c1), lo = val(c2);
    if (hi<0 || lo<0) return false;
    out[k] = static_cast<char>((hi<<4) | lo);
  }
  return true;
}

std::optional<std::string> ExtractField1(const data::Row &row) {
  for (const auto &c : row.columns()) {
    if (c.name() == "field1") {
      // interpret bytes as UTF-8-ish string; if arbitrary binary, adjust as needed
      return c.value();
    }
  }
  return std::nullopt;
}

// Parse one sst_dump line like:
// '313233...' seq:174, type:1 => 0A8B10...
// Return (key_hex, val_hex).
bool ParseSstDumpLine(const std::string &line, std::string &key_hex, std::string &val_hex) {
  // Find the first quoted segment as key hex
  auto q1 = line.find('\'');
  if (q1 == std::string::npos) return false;
  auto q2 = line.find('\'', q1+1);
  if (q2 == std::string::npos || q2 <= q1+1) return false;
  key_hex = line.substr(q1+1, q2 - (q1+1));
  // Find the "=>" separator
  auto arrow = line.find("=>", q2);
  if (arrow == std::string::npos) return false;
  // Value hex is after "=>"
  val_hex = line.substr(arrow + 2);
  // trim spaces
  auto ltrim = [](std::string &s){
    size_t i=0; while (i<s.size() && std::isspace((unsigned char)s[i])) ++i; s.erase(0,i);
  };
  auto rtrim = [](std::string &s){
    size_t j=s.size(); while (j>0 && std::isspace((unsigned char)s[j-1])) --j; s.erase(j);
  };
  ltrim(val_hex); rtrim(val_hex);
  return true;
}

} // namespace

int main(int argc, char **argv) {
  std::string out_text_path;
  std::string out_sst_path;

  // Parse args
  for (int i=1; i<argc; ++i) {
    std::string a = argv[i];
    if (a == "--out" && i+1<argc) {
      out_text_path = argv[++i];
    } else if (a.rfind("--build-sst=", 0) == 0) {
      out_sst_path = a.substr(std::string("--build-sst=").size());
    } else {
      std::cerr << "Unknown arg: " << a << "\n";
      std::cerr << "Usage: " << argv[0] << " --out index.txt | --build-sst=index.sst\n";
      return 2;
    }
  }
  if (out_text_path.empty() && out_sst_path.empty()) {
    std::cerr << "Please specify either --out <textfile> or --build-sst=<sstfile>\n";
    return 2;
  }
  if (!out_text_path.empty() && !out_sst_path.empty()) {
    std::cerr << "Choose ONE output mode: --out OR --build-sst\n";
    return 2;
  }

  std::ofstream text_out;
  if (!out_text_path.empty()) {
    text_out.open(out_text_path, std::ios::binary);
    if (!text_out) {
      std::cerr << "Cannot open text output: " << out_text_path << "\n";
      return 2;
    }
  }

  // (Optional) init RocksDB SstFileWriter if requested.
  // std::unique_ptr<rocksdb::SstFileWriter> sst_writer;
  // if (!out_sst_path.empty()) {
  //   rocksdb::EnvOptions env_opts;
  //   rocksdb::Options opts; opts.compression = rocksdb::kNoCompression;
  //   sst_writer.reset(new rocksdb::SstFileWriter(env_opts, opts));
  //   auto st = sst_writer->Open(out_sst_path);
  //   if (!st.ok()) { std::cerr << "SstFileWriter open failed: " << st.ToString() << "\n"; return 2; }
  // }

  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);

  size_t lines = 0, ok = 0, skipped = 0, nofield = 0, parsefail = 0;
  std::string line, key_hex, val_hex, key_bytes, val_bytes;

  while (std::getline(std::cin, line)) {
    ++lines;
    if (line.find("type:1") == std::string::npos) {
      ++skipped; continue; // only index puts
    }
    if (!ParseSstDumpLine(line, key_hex, val_hex)) {
      ++parsefail; continue;
    }
    if (!HexToBytes(key_hex, key_bytes) || !HexToBytes(val_hex, val_bytes)) {
      ++parsefail; continue;
    }

    // original key as string
    std::string orig_key = key_bytes; // assume ASCII-ish; adjust if binary keys
    data::Row row;
    if (!row.ParseFromString(val_bytes)) {
      ++parsefail; continue;
    }
    auto f1 = ExtractField1(row);
    if (!f1.has_value()) {
      ++nofield; continue;
    }
    // build index key
    std::string index_key;
    index_key.reserve(f1->size() + 3 + orig_key.size());
    index_key.append(*f1);
    index_key.append("$$$");
    index_key.append(orig_key);

    // Write it
    if (!out_text_path.empty()) {
      text_out.write(index_key.data(), static_cast<std::streamsize>(index_key.size()));
      text_out.put('\n');
      if (!text_out) { std::cerr << "write failed\n"; return 3; }
    } else {
      // RocksDB SST empty value
      // auto s = sst_writer->Put(index_key, "");
      // if (!s.ok()) { std::cerr << "SST Put failed: " << s.ToString() << "\n"; return 3; }
    }

    ++ok;
  }

  // if (sst_writer) {
  //   auto s = sst_writer->Finish();
  //   if (!s.ok()) { std::cerr << "SST Finish failed: " << s.ToString() << "\n"; return 3; }
  // }

  if (text_out.is_open()) text_out.flush();

  std::cerr << "Done. lines=" << lines
            << " indexed=" << ok
            << " skipped_non_put=" << skipped
            << " missing_field1=" << nofield
            << " parse_fail=" << parsefail
            << "\n";
  return 0;
}