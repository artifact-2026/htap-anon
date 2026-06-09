// jobs/split.cpp
//
// Resource-usage experiment for the "split rows into column groups" data
// transformation. This job exercises the transformation in isolation, with
// no RocksDB / LSM machinery in the loop, so that an external resource
// monitor (see the slack-meter project) can attribute CPU, disk-I/O, and RAM
// cost to the transform step itself rather than to compaction, flush, or
// other DB-engine activity.
//
// The job runs in two phases:
//
//   Phase 1 ("generate")
//     Produces synthetic rows the *same way the YCSB DB backends do* --
//     via CoreWorkload::NextSequenceKey() / BuildProtoRecord(), exactly as
//     Client::DoInsert() does for inputdataformat=protobuf -- and spills
//     them to a sequence of flat data files on disk, rolling over to a new
//     file once the current one reaches --file_size bytes.
//
//   Phase 2 ("split")
//     Reads --batch data files at a time, parses each row, splits its
//     columns into --groups column groups using the real Mycelium
//     Distributor transform (the same mycelium::Distributor /
//     mycelium::ProtobufParser / mycelium::ProtobufBytesRowEncoder machinery
//     that TestSplitting wires into RocksDB via SchemaDescriptor -- see
//     db/test_splitting.cc), and writes the resulting per-group rows back
//     out, one output stream per column group.
//
// Run `--phase generate`, `--phase split`, or `--phase both` (default) to
// run one or both phases independently -- e.g. generate the corpus once,
// then run the split phase repeatedly under the resource monitor.
//
// On-disk record framing (read by phase 2, written by both phases):
//
//   [u32 LE key_len][key bytes][u32 LE value_len][value bytes] ...
//
// repeated until EOF. `value` is a serialized data::ByteRow in the phase-1
// (input) files, and a serialized data::BytesRow-shaped buffer (one stream
// per column group, produced by mycelium::ProtobufBytesRowEncoder) in the
// phase-2 (output) files. This is a deliberately simple, dependency-free
// framing -- it carries the row's key alongside its bytes so the split phase
// can preserve key->row association across the transformation, without
// depending on any particular protobuf message shape for the envelope.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "data.pb.h"

#include "core/core_workload.h"
#include "core/properties.h"
#include "core/utils.h"

#include "mycelium/distributor.h"
#include "mycelium/protobuf_encoder.h"
#include "mycelium/protobuf_parser.h"
#include "mycelium/transformer.h"

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Command-line options
// ---------------------------------------------------------------------------
struct Options {
  std::string phase = "both";  // "generate" | "split" | "both"

  // Phase 1 ("generate") options.
  std::string spec_file;                          // -P <workload spec>
  uint64_t    file_size = 64ull * 1024 * 1024;     // bytes per data file before rollover

  // Phase 2 ("split") options.
  int batch  = 4;  // number of input data files processed together per round
  int groups = 2;  // number of column groups to split each row into

  // Shared options.
  std::string data_dir    = "./split_job_data";  // phase 1 writes here / phase 2 reads from here
  std::string out_dir     = "./split_job_out";   // phase 2 writes split output here
  std::string file_prefix = "rows";              // base name for data files
};

void Usage(const char *prog) {
  std::cerr
      << "Usage: " << prog << " [options]\n"
      << "\n"
      << "  --phase <generate|split|both>   Which phase(s) to run (default: both)\n"
      << "\n"
      << "  Phase 1 (\"generate\") options:\n"
      << "    -P <spec>                     Workload spec file, same format/semantics\n"
      << "                                  as ycsb_test's -P (provides recordcount,\n"
      << "                                  fieldcount, fieldlength, keylength, ...).\n"
      << "                                  Required when running phase \"generate\".\n"
      << "    --file_size <bytes>           Target size of each generated data file\n"
      << "                                  before rolling over to the next one.\n"
      << "                                  (default: 67108864 = 64 MiB)\n"
      << "\n"
      << "  Phase 2 (\"split\") options:\n"
      << "    --batch <N>                   Number of input data files read and\n"
      << "                                  processed together per round. (default: 4)\n"
      << "    --groups <N>                  Number of column groups each row is split\n"
      << "                                  into. (default: 2)\n"
      << "\n"
      << "  Shared options:\n"
      << "    --data_dir <dir>              Directory phase 1 writes data files to,\n"
      << "                                  and phase 2 reads them from.\n"
      << "                                  (default: ./split_job_data)\n"
      << "    --out_dir <dir>               Directory phase 2 writes split output\n"
      << "                                  files to. (default: ./split_job_out)\n"
      << "    --prefix <name>               Base name for generated/consumed data\n"
      << "                                  files. (default: \"rows\")\n";
}

bool ParseArgs(int argc, char **argv, Options &o) {
  auto next = [&](int &i, std::string *dst) -> bool {
    if (i + 1 >= argc) return false;
    *dst = argv[++i];
    return true;
  };

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--phase") {
      if (!next(i, &o.phase)) return false;
    } else if (a == "-P" || a == "--spec") {
      if (!next(i, &o.spec_file)) return false;
    } else if (a == "--data_dir") {
      if (!next(i, &o.data_dir)) return false;
    } else if (a == "--out_dir") {
      if (!next(i, &o.out_dir)) return false;
    } else if (a == "--prefix") {
      if (!next(i, &o.file_prefix)) return false;
    } else if (a == "--file_size") {
      std::string v;
      if (!next(i, &v)) return false;
      o.file_size = std::strtoull(v.c_str(), nullptr, 10);
    } else if (a == "--batch") {
      std::string v;
      if (!next(i, &v)) return false;
      o.batch = std::atoi(v.c_str());
    } else if (a == "--groups") {
      std::string v;
      if (!next(i, &v)) return false;
      o.groups = std::atoi(v.c_str());
    } else if (a == "-h" || a == "--help") {
      return false;
    } else {
      std::cerr << "Unknown argument: " << a << "\n";
      return false;
    }
  }

  if (o.phase != "generate" && o.phase != "split" && o.phase != "both") {
    std::cerr << "--phase must be one of: generate, split, both\n";
    return false;
  }
  if ((o.phase == "generate" || o.phase == "both") && o.spec_file.empty()) {
    std::cerr << "Phase \"generate\" requires -P <workload spec>\n";
    return false;
  }
  if (o.file_size == 0) {
    std::cerr << "--file_size must be > 0\n";
    return false;
  }
  if (o.batch <= 0) {
    std::cerr << "--batch must be > 0\n";
    return false;
  }
  if (o.groups <= 0) {
    std::cerr << "--groups must be > 0\n";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Length-delimited [key, value] record framing shared by both phases.
//
//   [u32 LE key_len][key bytes][u32 LE value_len][value bytes]
// ---------------------------------------------------------------------------

// Appends one record to `out`. Returns the number of bytes written, which
// callers use to track when a file has crossed its target size.
uint64_t WriteRecord(std::ofstream &out, const std::string &key, const std::string &value) {
  const uint32_t klen = static_cast<uint32_t>(key.size());
  const uint32_t vlen = static_cast<uint32_t>(value.size());
  out.write(reinterpret_cast<const char *>(&klen), sizeof(klen));
  out.write(key.data(), static_cast<std::streamsize>(key.size()));
  out.write(reinterpret_cast<const char *>(&vlen), sizeof(vlen));
  out.write(value.data(), static_cast<std::streamsize>(value.size()));
  return sizeof(klen) + key.size() + sizeof(vlen) + value.size();
}

// Reads one record from `in`. Returns false at a clean EOF (no more records)
// or on any I/O error partway through a record.
bool ReadRecord(std::ifstream &in, std::string &key, std::string &value) {
  uint32_t klen = 0;
  if (!in.read(reinterpret_cast<char *>(&klen), sizeof(klen))) return false;

  key.resize(klen);
  if (klen > 0 && !in.read(&key[0], klen)) return false;

  uint32_t vlen = 0;
  if (!in.read(reinterpret_cast<char *>(&vlen), sizeof(vlen))) return false;

  value.resize(vlen);
  if (vlen > 0 && !in.read(&value[0], vlen)) return false;

  return true;
}

// ---------------------------------------------------------------------------
// Phase 1: generate
//
// Produces rows the same way the YCSB DB backends do: load a workload spec
// into a CoreWorkload, then call NextSequenceKey() / BuildProtoRecord() in a
// loop -- mirroring Client::DoInsert()'s "protobuf" path -- and spill the
// resulting (key, serialized data::ByteRow) pairs to flat data files, each
// capped at --file_size bytes.
// ---------------------------------------------------------------------------
int RunGeneratePhase(const Options &opt) {
  utils::Properties props;
  {
    std::ifstream spec(opt.spec_file);
    if (!spec.is_open()) {
      std::cerr << "[generate] cannot open workload spec: " << opt.spec_file << "\n";
      return 1;
    }
    try {
      props.Load(spec);
    } catch (const std::string &msg) {
      std::cerr << "[generate] " << msg << "\n";
      return 1;
    }
  }

  // This job always writes data::ByteRow protobuf records to flat files
  // (regardless of what the spec says) so phase 2 has one well-defined wire
  // format to parse. fieldcount / fieldlength / recordcount / keylength etc.
  // still come from the spec, exactly as they would for ycsb_test.
  props.SetProperty("inputdataformat", "protobuf");
  // CoreWorkload::Init calls stoi() on these with no default -- supply
  // fallbacks so specs that omit totalrecordcount (e.g. test_basic.spec)
  // don't crash.
  if (props.GetProperty(ycsbc::CoreWorkload::TOTAL_RECORD_COUNT_PROPERTY, "").empty()) {
    props.SetProperty(ycsbc::CoreWorkload::TOTAL_RECORD_COUNT_PROPERTY,
                      props.GetProperty(ycsbc::CoreWorkload::RECORD_COUNT_PROPERTY, "1000"));
  }

  ycsbc::CoreWorkload wl;
  wl.Init(props);

  const uint64_t record_count = std::strtoull(
      props.GetProperty(ycsbc::CoreWorkload::RECORD_COUNT_PROPERTY, "1000").c_str(), nullptr, 10);

  std::error_code ec;
  fs::create_directories(opt.data_dir, ec);
  if (ec) {
    std::cerr << "[generate] cannot create data dir " << opt.data_dir << ": " << ec.message() << "\n";
    return 1;
  }

  int file_index = 0;
  uint64_t bytes_in_file = 0;
  uint64_t files_written = 0;
  uint64_t rows_written = 0;

  std::ofstream out;
  auto open_next_file = [&]() -> bool {
    if (out.is_open()) out.close();
    const std::string path =
        opt.data_dir + "/" + opt.file_prefix + "." + std::to_string(file_index++) + ".dat";
    out.open(path, std::ios::binary | std::ios::trunc);
    bytes_in_file = 0;
    if (!out) {
      std::cerr << "[generate] cannot open output file: " << path << "\n";
      return false;
    }
    ++files_written;
    std::cout << "[generate] writing -> " << path << "\n";
    return true;
  };

  if (!open_next_file()) return 1;

  const auto t0 = std::chrono::steady_clock::now();

  for (uint64_t i = 0; i < record_count; ++i) {
    const std::string key = wl.NextSequenceKey();

    data::ByteRow row;
    wl.BuildProtoRecord(row);
    std::string serialized;
    row.SerializeToString(&serialized);

    bytes_in_file += WriteRecord(out, key, serialized);
    ++rows_written;

    if (bytes_in_file >= opt.file_size && i + 1 < record_count) {
      if (!open_next_file()) return 1;
    }
  }
  out.close();

  const auto t1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  std::cout << "[generate] done: " << rows_written << " row(s) across " << files_written
            << " file(s) in " << opt.data_dir << " (" << secs << "s, "
            << (secs > 0 ? static_cast<double>(rows_written) / secs : 0.0) << " rows/s)\n";
  return 0;
}

// ---------------------------------------------------------------------------
// Phase 2: split
//
// Reads --batch data files at a time, parses each row as a data::ByteRow,
// splits its columns into --groups column groups via the real Mycelium
// Distributor transform, and serializes + writes each group's row to its own
// output file. Column groups are computed once, the first time a row is
// seen, by round-robin assignment of column indices -- the same interleaved
// pattern db/test_splitting.cc uses when wiring a Distributor into RocksDB.
// ---------------------------------------------------------------------------

// Partitions column indices [0, field_count) into `num_groups` round-robin
// groups, e.g. field_count=5, num_groups=2 -> {{0,2,4}, {1,3}}. Caps
// num_groups at field_count (a group cannot be empty -- mycelium::Distributor
// rejects empty split groups).
std::vector<std::vector<int>> ComputeColumnGroups(int field_count, int num_groups) {
  std::vector<std::vector<int>> groups;
  if (field_count <= 0) return groups;

  num_groups = std::min(num_groups, field_count);
  groups.resize(static_cast<size_t>(num_groups));
  for (int col = 0; col < field_count; ++col) {
    groups[static_cast<size_t>(col % num_groups)].push_back(col);
  }
  return groups;
}

int RunSplitPhase(const Options &opt) {
  // Discover the data files written by phase 1 (or a prior "generate" run).
  std::vector<fs::path> input_files;
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(opt.data_dir, ec)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    if (entry.path().extension() == ".dat" && name.rfind(opt.file_prefix + ".", 0) == 0) {
      input_files.push_back(entry.path());
    }
  }
  if (ec) {
    std::cerr << "[split] cannot read data dir " << opt.data_dir << ": " << ec.message() << "\n";
    return 1;
  }
  if (input_files.empty()) {
    std::cerr << "[split] no \"" << opt.file_prefix << ".*.dat\" files found in " << opt.data_dir
              << " -- run phase \"generate\" first\n";
    return 1;
  }
  std::sort(input_files.begin(), input_files.end());

  fs::create_directories(opt.out_dir, ec);
  if (ec) {
    std::cerr << "[split] cannot create out dir " << opt.out_dir << ": " << ec.message() << "\n";
    return 1;
  }

  // Parses the data::ByteRow wire format written by phase 1 into the
  // engine-agnostic ParsedRow representation that mycelium transformers
  // operate on.
  mycelium::ProtobufParser parser(std::make_unique<data::ByteRow>());

  // The Distributor, per-group encoders, column-group layout, and output
  // streams are all built lazily from the first successfully-parsed row,
  // since that's when we first learn how many columns each row carries.
  // (Rows generated by phase 1 all share the same field count.)
  std::shared_ptr<mycelium::Distributor> distributor;
  std::vector<std::unique_ptr<mycelium::ProtobufBytesRowEncoder>> encoders;
  std::vector<std::vector<int>> column_groups;
  std::vector<std::ofstream> group_out;

  auto setup_for_row = [&](const mycelium::ParsedRow &row) -> bool {
    column_groups = ComputeColumnGroups(static_cast<int>(row.size()), opt.groups);
    if (column_groups.empty()) {
      std::cerr << "[split] cannot split a " << row.size() << "-column row into " << opt.groups
                << " group(s)\n";
      return false;
    }
    distributor = std::make_shared<mycelium::Distributor>(column_groups);

    encoders.reserve(column_groups.size());
    group_out.resize(column_groups.size());
    for (size_t g = 0; g < column_groups.size(); ++g) {
      encoders.push_back(std::make_unique<mycelium::ProtobufBytesRowEncoder>(column_groups[g].size()));

      const std::string path =
          opt.out_dir + "/" + opt.file_prefix + ".group" + std::to_string(g) + ".dat";
      group_out[g].open(path, std::ios::binary | std::ios::trunc);
      if (!group_out[g]) {
        std::cerr << "[split] cannot open output file: " << path << "\n";
        return false;
      }
      std::cout << "[split] group " << g << " (" << column_groups[g].size()
                << " column(s)) -> " << path << "\n";
    }

    std::cout << "[split] " << row.size() << "-column rows -> " << column_groups.size()
              << " group(s)\n";
    return true;
  };

  uint64_t rows_processed = 0;
  uint64_t rows_failed = 0;
  uint64_t batch_index = 0;

  const auto t0 = std::chrono::steady_clock::now();

  for (size_t start = 0; start < input_files.size(); start += static_cast<size_t>(opt.batch)) {
    const size_t end = std::min(start + static_cast<size_t>(opt.batch), input_files.size());
    std::cout << "[split] batch " << batch_index++ << ": files " << (start + 1) << "-" << end
              << " of " << input_files.size() << "\n";

    for (size_t fi = start; fi < end; ++fi) {
      std::ifstream in(input_files[fi], std::ios::binary);
      if (!in) {
        std::cerr << "[split] cannot open " << input_files[fi] << "\n";
        continue;
      }

      std::string key, value;
      while (ReadRecord(in, key, value)) {
        const mycelium::ByteBuffer buf(value.begin(), value.end());
        mycelium::Result<mycelium::ParsedRow> parsed = parser.Parse(buf);
        if (!parsed.ok()) {
          ++rows_failed;
          continue;
        }
        const mycelium::ParsedRow &row = *parsed;

        if (!distributor && !setup_for_row(row)) return 1;

        const std::vector<mycelium::ParsedRow> outputs = distributor->Transform(key, row);
        for (size_t g = 0; g < outputs.size() && g < group_out.size(); ++g) {
          for (const mycelium::ByteBuffer &enc : encoders[g]->Serialize(outputs[g])) {
            const std::string out_value(enc.begin(), enc.end());
            WriteRecord(group_out[g], key, out_value);
          }
        }
        ++rows_processed;
      }
    }
  }

  for (auto &s : group_out) s.close();

  const auto t1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  std::cout << "[split] done: " << rows_processed << " row(s) split, " << rows_failed
            << " failed to parse, across " << input_files.size() << " input file(s) in "
            << opt.out_dir << " (" << secs << "s, "
            << (secs > 0 ? static_cast<double>(rows_processed) / secs : 0.0) << " rows/s)\n";

  return (rows_processed == 0 && rows_failed > 0) ? 1 : 0;
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!ParseArgs(argc, argv, opt)) {
    Usage(argv[0]);
    return 1;
  }

  if (opt.phase == "generate" || opt.phase == "both") {
    if (int rc = RunGeneratePhase(opt)) return rc;
  }
  if (opt.phase == "split" || opt.phase == "both") {
    if (int rc = RunSplitPhase(opt)) return rc;
  }
  return 0;
}
