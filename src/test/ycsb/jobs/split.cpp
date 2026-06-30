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
//     Reads all data files in --data_dir in --batch-file increments, parses
//     each row, splits its columns into --groups column groups using the real
//     Mycelium Distributor transform (the same mycelium::Distributor /
//     mycelium::ProtobufParser / mycelium::ProtobufBytesRowEncoder machinery
//     that TestSplitting wires into RocksDB via SchemaDescriptor -- see
//     db/test_splitting.cc), and writes the resulting per-group rows to
//     output files. Prints "throughput mean: X  stddev: 0" on completion so
//     probe_split.py can parse it with the same regex as probe_rocksdb.py.
//
// Run `--phase generate`, `--phase split`, or `--phase both` (default).
// Typical workflow: generate data once, then run split repeatedly under the
// resource monitor (probe_split.py in the slack-meter project).
//
// On-disk record framing (read by phase 2, written by both phases):
//
//   [u32 LE key_len][key bytes][u32 LE value_len][value bytes] ...
//
// repeated until EOF. `value` is a serialized data::ByteRow in the phase-1
// (input) files, and a serialized data::BytesRow-shaped buffer (one stream
// per column group, produced by mycelium::ProtobufBytesRowEncoder) in the
// phase-2 (output) files.

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

#include <fcntl.h>
#include <unistd.h>
#include <cstring>

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
  std::string spec_file;                        // -P <workload spec>
  uint64_t    file_size = 64ull * 1024 * 1024;  // bytes per data file before rollover

  // Phase 2 ("split") options.
  int batch    = 4;    // kept for CLI compatibility; not used in duration-loop mode
  int groups   = 2;    // number of column groups to split each row into
  int duration = 600;  // seconds to run the split loop (0 = single pass)
  bool sync_output   = false;  // open output files with O_DSYNC (force per-write disk flush)
  bool direct_reads  = false;  // open source files with O_DIRECT (bypass page cache on reads)

  // Shared options.
  std::string data_dir    = "./split_job_data";
  std::string out_dir     = "./split_job_out";
  std::string file_prefix = "rows";
};

void Usage(const char *prog) {
  std::cerr
      << "Usage: " << prog << " [options]\n"
      << "\n"
      << "  --phase <generate|split|both>   Which phase(s) to run (default: both)\n"
      << "\n"
      << "  Phase 1 (\"generate\") options:\n"
      << "    -P <spec>                     Workload spec file (provides recordcount,\n"
      << "                                  fieldcount, fieldlength, keylength, ...).\n"
      << "                                  Required when running phase \"generate\".\n"
      << "    --file_size <bytes>           Target size per data file before rollover.\n"
      << "                                  (default: 67108864 = 64 MiB)\n"
      << "\n"
      << "  Phase 2 (\"split\") options:\n"
      << "    --batch <N>                   (legacy, unused in duration mode) (default: 4)\n"
      << "    --groups <N>                  Number of column groups to split into.\n"
      << "                                  (default: 2)\n"
      << "    --duration <secs>             How long to run the split loop. Files are\n"
      << "                                  cycled repeatedly until the deadline. 0 = single\n"
      << "                                  pass. (default: 600)\n"
      << "\n"
      << "  Shared options:\n"
      << "    --data_dir <dir>              Phase 1 writes here; phase 2 reads from here.\n"
      << "                                  (default: ./split_job_data)\n"
      << "    --out_dir <dir>               Phase 2 writes split output here.\n"
      << "                                  (default: ./split_job_out)\n"
      << "    --prefix <name>               Base name for data files. (default: \"rows\")\n"
      << "    --sync-output                 Open output files with O_DSYNC so every write\n"
      << "                                  blocks until data reaches the storage device.\n"
      << "                                  Use this to make split IO visible to the slack\n"
      << "                                  experiment (otherwise writes are OS-buffered).\n"
      << "    --direct-reads                Open source files with O_DIRECT so reads bypass\n"
      << "                                  the OS page cache entirely. Essential when RAM\n"
      << "                                  exceeds the source file pool size (files would\n"
      << "                                  otherwise be served from cache after one pass).\n";
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
    } else if (a == "--duration") {
      std::string v;
      if (!next(i, &v)) return false;
      o.duration = std::atoi(v.c_str());
    } else if (a == "--sync-output") {
      o.sync_output = true;
    } else if (a == "--direct-reads") {
      o.direct_reads = true;
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

uint64_t WriteRecord(std::ofstream &out, const std::string &key, const std::string &value) {
  const uint32_t klen = static_cast<uint32_t>(key.size());
  const uint32_t vlen = static_cast<uint32_t>(value.size());
  out.write(reinterpret_cast<const char *>(&klen), sizeof(klen));
  out.write(key.data(), static_cast<std::streamsize>(key.size()));
  out.write(reinterpret_cast<const char *>(&vlen), sizeof(vlen));
  out.write(value.data(), static_cast<std::streamsize>(value.size()));
  return sizeof(klen) + key.size() + sizeof(vlen) + value.size();
}

// Variant used when --sync-output is set: writes directly via fd (no userspace
// buffering) so O_DSYNC takes effect on every write() call.
uint64_t WriteRecordFd(int fd, const std::string &key, const std::string &value) {
  const uint32_t klen = static_cast<uint32_t>(key.size());
  const uint32_t vlen = static_cast<uint32_t>(value.size());
  ::write(fd, &klen, sizeof(klen));
  ::write(fd, key.data(), key.size());
  ::write(fd, &vlen, sizeof(vlen));
  ::write(fd, value.data(), value.size());
  return sizeof(klen) + key.size() + sizeof(vlen) + value.size();
}

// ---------------------------------------------------------------------------
// DirectReader: O_DIRECT source-file reader.
//
// O_DIRECT bypasses the OS page cache so reads always come from the storage
// device regardless of how much RAM the machine has.  The kernel requires:
//   - fd offset aligned to logical block size  (guaranteed: we read in whole
//     kBufSize chunks, which is a power-of-two multiple of 4 KiB)
//   - buffer address aligned to kAlign         (guaranteed: posix_memalign)
//   - request length a multiple of kAlign      (guaranteed: kBufSize)
//
// The last read of a file may return fewer than kBufSize bytes (kernel
// truncates at EOF); we record exactly how many bytes came back and stop
// consuming the buffer at that boundary.
// ---------------------------------------------------------------------------
struct DirectReader {
  static constexpr size_t kAlign   = 4096;
  static constexpr size_t kBufSize = 1024 * 1024;  // 1 MiB

  int    fd_      = -1;
  char  *buf_     = nullptr;
  size_t buf_end_ = 0;   // valid bytes in buf_
  size_t buf_pos_ = 0;   // next byte to consume
  bool   eof_     = false;

  bool Open(const std::string &path) {
#ifdef O_DIRECT
    fd_ = ::open(path.c_str(), O_RDONLY | O_DIRECT);
#else
    fd_ = ::open(path.c_str(), O_RDONLY);
#endif
    if (fd_ < 0) return false;
    if (::posix_memalign(reinterpret_cast<void **>(&buf_), kAlign, kBufSize) != 0) {
      ::close(fd_); fd_ = -1; return false;
    }
    buf_end_ = buf_pos_ = 0;
    eof_ = false;
    return true;
  }

  void Close() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    if (buf_)    { free(buf_); buf_ = nullptr; }
    buf_end_ = buf_pos_ = 0;
    eof_ = false;
  }

  ~DirectReader() { Close(); }

  // Read exactly n bytes into dst; returns false if EOF/error before n bytes.
  bool Read(char *dst, size_t n) {
    size_t done = 0;
    while (done < n) {
      if (buf_pos_ >= buf_end_) {
        if (eof_) return false;
        ssize_t got = ::read(fd_, buf_, kBufSize);
        if (got <= 0) { eof_ = true; return false; }
        buf_end_ = static_cast<size_t>(got);
        buf_pos_ = 0;
      }
      size_t take = std::min(buf_end_ - buf_pos_, n - done);
      std::memcpy(dst + done, buf_ + buf_pos_, take);
      buf_pos_ += take;
      done     += take;
    }
    return true;
  }
};

bool ReadRecordDirect(DirectReader &r, std::string &key, std::string &value) {
  uint32_t klen = 0;
  if (!r.Read(reinterpret_cast<char *>(&klen), sizeof(klen))) return false;
  key.resize(klen);
  if (klen > 0 && !r.Read(&key[0], klen)) return false;
  uint32_t vlen = 0;
  if (!r.Read(reinterpret_cast<char *>(&vlen), sizeof(vlen))) return false;
  value.resize(vlen);
  if (vlen > 0 && !r.Read(&value[0], vlen)) return false;
  return true;
}

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
  uint64_t rows_written  = 0;

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

  const auto t1   = std::chrono::steady_clock::now();
  const double sec = std::chrono::duration<double>(t1 - t0).count();
  std::cout << "[generate] done: " << rows_written << " row(s) across " << files_written
            << " file(s) in " << opt.data_dir << " (" << sec << "s, "
            << (sec > 0 ? static_cast<double>(rows_written) / sec : 0.0) << " rows/s)\n";
  return 0;
}

// ---------------------------------------------------------------------------
// Phase 2: split
// ---------------------------------------------------------------------------

// Partitions [0, field_count) into num_groups round-robin groups.
// e.g. field_count=5, num_groups=2 -> {{0,2,4},{1,3}}.
std::vector<std::vector<int>> ComputeColumnGroups(int field_count, int num_groups) {
  std::vector<std::vector<int>> groups;
  if (field_count <= 0) return groups;
  num_groups = std::min(num_groups, field_count);
  groups.resize(static_cast<size_t>(num_groups));
  for (int col = 0; col < field_count; ++col)
    groups[static_cast<size_t>(col % num_groups)].push_back(col);
  return groups;
}

int RunSplitPhase(const Options &opt) {
  // Discover input data files.
  std::vector<fs::path> input_files;
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(opt.data_dir, ec)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    if (entry.path().extension() == ".dat" && name.rfind(opt.file_prefix + ".", 0) == 0)
      input_files.push_back(entry.path());
  }
  if (ec) {
    std::cerr << "[split] cannot read data dir " << opt.data_dir << ": " << ec.message() << "\n";
    return 1;
  }
  if (input_files.empty()) {
    std::cerr << "[split] no \"" << opt.file_prefix << ".*.dat\" files found in "
              << opt.data_dir << " -- run phase \"generate\" first\n";
    return 1;
  }
  std::sort(input_files.begin(), input_files.end());

  // Clear the output directory before each run so stale files from a previous
  // probe don't accumulate. Only removes files matching <prefix>.group*.dat so
  // other content in the directory (if any) is left untouched.
  if (fs::exists(opt.out_dir)) {
    for (const auto &entry : fs::directory_iterator(opt.out_dir, ec)) {
      if (!entry.is_regular_file()) continue;
      const std::string name = entry.path().filename().string();
      if (entry.path().extension() == ".dat" && name.rfind(opt.file_prefix + ".group", 0) == 0) {
        fs::remove(entry.path(), ec);
        if (ec)
          std::cerr << "[split] warning: could not remove " << entry.path()
                    << ": " << ec.message() << "\n";
      }
    }
  }
  fs::create_directories(opt.out_dir, ec);
  if (ec) {
    std::cerr << "[split] cannot create out dir " << opt.out_dir << ": " << ec.message() << "\n";
    return 1;
  }

  mycelium::ProtobufParser parser(std::make_unique<data::ByteRow>());

  // Distributor/encoders/outputs are initialized lazily from the first row.
  std::shared_ptr<mycelium::Distributor> distributor;
  std::vector<std::unique_ptr<mycelium::ProtobufBytesRowEncoder>> encoders;
  // Buffered outputs (default path).
  std::vector<std::ofstream> group_out;
  // Sync outputs: raw fds opened with O_DSYNC (used when opt.sync_output).
  std::vector<int> group_fds;

  auto init_transform = [&](const mycelium::ParsedRow &row) -> bool {
    auto column_groups = ComputeColumnGroups(static_cast<int>(row.size()), opt.groups);
    if (column_groups.empty()) {
      std::cerr << "[split] cannot split a " << row.size() << "-column row into "
                << opt.groups << " group(s)\n";
      return false;
    }
    distributor = std::make_shared<mycelium::Distributor>(column_groups);
    encoders.reserve(column_groups.size());
    if (opt.sync_output) {
      group_fds.resize(column_groups.size(), -1);
    } else {
      group_out.resize(column_groups.size());
    }
    for (size_t g = 0; g < column_groups.size(); ++g) {
      encoders.push_back(
          std::make_unique<mycelium::ProtobufBytesRowEncoder>(column_groups[g].size()));
      const std::string path =
          opt.out_dir + "/" + opt.file_prefix + ".group" + std::to_string(g) + ".dat";
      if (opt.sync_output) {
        int flags = O_WRONLY | O_CREAT | O_TRUNC | O_DSYNC;
        group_fds[g] = ::open(path.c_str(), flags, 0644);
        if (group_fds[g] < 0) {
          std::cerr << "[split] cannot open output file (O_DSYNC): " << path << "\n";
          return false;
        }
      } else {
        group_out[g].open(path, std::ios::binary | std::ios::trunc);
        if (!group_out[g]) {
          std::cerr << "[split] cannot open output file: " << path << "\n";
          return false;
        }
      }
      std::cout << "[split] group " << g << " (" << column_groups[g].size()
                << " column(s)) -> " << path
                << (opt.sync_output ? " [O_DSYNC]" : "") << "\n";
    }
    std::cout << "[split] " << row.size() << "-column rows -> " << column_groups.size()
              << " group(s)\n";
    return true;
  };

  uint64_t rows_processed = 0;
  uint64_t rows_failed    = 0;
  uint64_t passes         = 0;

  const auto t0       = std::chrono::steady_clock::now();
  const bool timed    = (opt.duration > 0);
  const auto deadline = t0 + std::chrono::seconds(opt.duration);

  // Loop over the input files repeatedly until the deadline (or once if
  // duration == 0).
  while (true) {
    ++passes;
    bool hit_deadline = false;

    // process_record: shared logic for both buffered and direct-IO paths.
    auto process_record = [&](const std::string &key, const std::string &value) -> bool {
      mycelium::Result<mycelium::ParsedRow> parsed = parser.Parse(value);
      if (!parsed.ok()) { ++rows_failed; return true; }
      const mycelium::ParsedRow &row = *parsed;

      if (!distributor && !init_transform(row)) return false;  // fatal

      const std::vector<mycelium::ParsedRow> outputs = distributor->Transform(key, row);
      const size_t n_groups = opt.sync_output ? group_fds.size() : group_out.size();
      for (size_t g = 0; g < outputs.size() && g < n_groups; ++g) {
        for (const mycelium::ByteBuffer &enc : encoders[g]->Serialize(outputs[g])) {
          const std::string out_value(enc.begin(), enc.end());
          if (opt.sync_output)
            WriteRecordFd(group_fds[g], key, out_value);
          else
            WriteRecord(group_out[g], key, out_value);
        }
      }
      ++rows_processed;
      return true;
    };

    for (const auto &fpath : input_files) {
      // Check deadline between files.
      if (timed && std::chrono::steady_clock::now() >= deadline) {
        hit_deadline = true;
        break;
      }

      std::string key, value;
      if (opt.direct_reads) {
        DirectReader dr;
        if (!dr.Open(fpath.string())) {
          std::cerr << "[split] cannot open (O_DIRECT) " << fpath << "\n";
          continue;
        }
        while (ReadRecordDirect(dr, key, value))
          if (!process_record(key, value)) return 1;
      } else {
        std::ifstream in(fpath, std::ios::binary);
        if (!in) {
          std::cerr << "[split] cannot open " << fpath << "\n";
          continue;
        }
        while (ReadRecord(in, key, value))
          if (!process_record(key, value)) return 1;
      }
    }

    // Stop after one pass if not in timed mode, or if the deadline was hit.
    if (!timed || hit_deadline) break;
  }

  for (auto &s : group_out) s.close();
  for (int fd : group_fds) if (fd >= 0) ::close(fd);

  const auto t1    = std::chrono::steady_clock::now();
  const double sec = std::chrono::duration<double>(t1 - t0).count();
  const double rows_per_sec = sec > 0.0 ? static_cast<double>(rows_processed) / sec : 0.0;

  // Emit ycsb_test-compatible line so probe_split.py can parse it with the
  // same regex as probe_rocksdb.py.
  std::cout << "throughput mean: " << rows_per_sec << "  stddev: 0\n";

  std::cout << "[split] elapsed: " << sec << "s\n";
  std::cout << "[split] done: " << rows_processed << " row(s) split, " << rows_failed
            << " failed to parse, " << passes << " pass(es) over " << input_files.size()
            << " input file(s) in " << opt.out_dir << " (" << sec << "s, "
            << rows_per_sec << " rows/s)\n";

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
