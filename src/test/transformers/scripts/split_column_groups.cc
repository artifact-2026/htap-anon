// split_column_groups.cc (revised for concrete data::Row)
// Reads length-delimited protobuf records of type data::Row from --input,
// splits each record's columns roughly in half into two data::Row records
// (preserving the same key), and writes them as two length-delimited streams
// to --out1 and --out2 respectively.
//
// Build (example):
//   c++ -std=c++17 -O2 -Wall split_column_groups.cc -o split_column_groups 
//       $(pkg-config --cflags --libs protobuf)
//
// Usage:
//   ./split_column_groups 
//     --input /path/to/input.binpb 
//     --out1  /path/to/output_A.binpb 
//     --out2  /path/to/output_B.binpb

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/util/delimited_message_util.h>

#include "data.pb.h"  // generated from your data.proto (package data)

namespace io = google::protobuf::io;

struct Args {
  std::string input_file;
  std::string out1_file;
  std::string out2_file;
};

static void usage(const char* prog) {
  std::cerr << "Usage: " << prog << " \\\n    --input INPUT.binpb \\\n    --out1  OUT_A.binpb \\\n    --out2  OUT_B.binpb\n";
}

static bool parseArgs(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto eat = [&](const char* flag, std::string* dst) -> bool {
      std::string f(flag);
      if (arg == f && i + 1 < argc) { *dst = argv[++i]; return true; }
      if (arg.rfind(f + std::string("="), 0) == 0) { *dst = arg.substr(f.size() + 1); return true; }
      return false;
    };
    if (eat("--input", &a.input_file)) continue;
    if (eat("--out1",  &a.out1_file)) continue;
    if (eat("--out2",  &a.out2_file)) continue;
    std::cerr << "Unknown arg: " << arg << "\n";
    return false;
  }
  if (a.input_file.empty() || a.out1_file.empty() || a.out2_file.empty()) return false;
  return true;
}

int main(int argc, char** argv) {
  Args args;
  if (!parseArgs(argc, argv, args)) { usage(argv[0]); return 1; }

  std::ifstream fin(args.input_file, std::ios::binary);
  if (!fin) {
    std::cerr << "Failed to open input: " << args.input_file << "\n";
    return 2;
  }
  std::ofstream foutA(args.out1_file, std::ios::binary);
  std::ofstream foutB(args.out2_file, std::ios::binary);
  if (!foutA || !foutB) {
    std::cerr << "Failed to open outputs: '" << args.out1_file
              << "' and/or '" << args.out2_file << "'\n";
    return 3;
  }

  io::IstreamInputStream zin(&fin);
  io::OstreamOutputStream zoutA(&foutA);
  io::OstreamOutputStream zoutB(&foutB);

  uint64_t count = 0;

  data::Row row;       // reuse across iterations
  data::Row rowA, rowB; // outputs reused & cleared each time

  while (true) {
    bool clean_eof = false;
    row.Clear();
    if (!google::protobuf::util::ParseDelimitedFromZeroCopyStream(&row, &zin, &clean_eof)) {
      if (clean_eof) break;  // normal EOF after last record
      std::cerr << "Parse error at record #" << count << "\n";
      return 4;
    }

    // Prepare outputs for this record
    rowA.Clear();
    rowB.Clear();

    const int n = row.columns_size();
    const int half = n / 2;        // A gets [0, half), B gets [half, n)

    // Optionally reserve to avoid reallocations
    rowA.mutable_columns()->Reserve(half);
    rowB.mutable_columns()->Reserve(n - half);

    for (int i = 0; i < n; ++i) {
      const data::Column& src = row.columns(i);
      data::Column* dst = (i < half) ? rowA.add_columns() : rowB.add_columns();
      dst->set_name(src.name());
      dst->set_value(src.value()); // bytes-safe copy
    }

    if (!google::protobuf::util::SerializeDelimitedToZeroCopyStream(rowA, &zoutA)) {
      std::cerr << "Serialize out1 failed at record #" << count << "\n";
      return 5;
    }
    if (!google::protobuf::util::SerializeDelimitedToZeroCopyStream(rowB, &zoutB)) {
      std::cerr << "Serialize out2 failed at record #" << count << "\n";
      return 6;
    }

    ++count;
  }

  std::cerr << "Done. Records processed: " << count << "\n";
  return 0;
}
