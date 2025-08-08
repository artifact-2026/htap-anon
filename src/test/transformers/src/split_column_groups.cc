#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <unordered_set>
#include <algorithm>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/util/delimited_message_util.h>

using google::protobuf::Descriptor;
using google::protobuf::DescriptorPool;
using google::protobuf::DynamicMessageFactory;
using google::protobuf::FileDescriptorSet;
using google::protobuf::Message;
using google::protobuf::Reflection;
using google::protobuf::FieldDescriptor;
namespace io = google::protobuf::io;

// Simple CLI parsing
struct Args {
  std::string input_file;
  std::string out_dir;
  std::string desc_file;
  std::string type_name;      // e.g., my.pkg.Record
  std::vector<std::string> key_fields; // comma-separated
};

static void usage(const char* prog) {
  std::cerr <<
    "Usage: " << prog << " \\\n"
    "  --input INPUT.binpb \\\n"
    "  --out_dir OUT_DIR \\\n"
    "  --desc schema.desc \\\n"
    "  --type my.pkg.Record \\\n"
    "  [--key_fields=field1,field2,...]\n";
}

static std::vector<std::string> splitComma(const std::string& s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start < s.size()) {
    size_t pos = s.find(',', start);
    if (pos == std::string::npos) pos = s.size();
    std::string token = s.substr(start, pos - start);
    if (!token.empty()) out.push_back(token);
    start = pos + 1;
  }
  return out;
}

static bool parseArgs(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto eat = [&](const char* flag, std::string* dst) -> bool {
      std::string f(flag);
      if (arg == f && i + 1 < argc) { *dst = argv[++i]; return true; }
      if (arg.rfind(f + "=", 0) == 0) { *dst = arg.substr(f.size() + 1); return true; }
      return false;
    };
    if (eat("--input", &a.input_file)) continue;
    if (eat("--out_dir", &a.out_dir)) continue;
    if (eat("--desc", &a.desc_file)) continue;
    if (eat("--type", &a.type_name)) continue;
    if (arg.rfind("--key_fields=", 0) == 0) {
      a.key_fields = splitComma(arg.substr(std::string("--key_fields=").size()));
      continue;
    }
    std::cerr << "Unknown arg: " << arg << "\n";
    return false;
  }
  if (a.input_file.empty() || a.out_dir.empty() || a.desc_file.empty() || a.type_name.empty()) {
    return false;
  }
  return true;
}

// Load FileDescriptorSet and build a DescriptorPool
static bool loadPool(const std::string& path, DescriptorPool& pool, std::unique_ptr<FileDescriptorSet>& backing) {
  backing = std::make_unique<google::protobuf::FileDescriptorSet>();
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::cerr << "Failed to open descriptor set: " << path << "\n";
    return false;
  }
  if (!backing->ParseFromIstream(&in)) {
    std::cerr << "Failed to parse descriptor set: " << path << "\n";
    return false;
  }

  for (int i = 0; i < backing->file_size(); ++i) {
    const auto& f = backing->file(i);
    if (pool.BuildFile(f) == nullptr) {
      std::cerr << "DescriptorPool::BuildFile failed for proto: " << f.name() << "\n";
      return false;
    }
  }
  
  return true;
}

// Copy a subset of fields from src to dst (same message type), keeping only fields in 'keep'
static void copySubset(const Message& src, Message& dst,
                       const std::unordered_set<const FieldDescriptor*>& keep) {
  const Reflection* ref = src.GetReflection();
  std::vector<const FieldDescriptor*> fields;
  ref->ListFields(src, &fields);
  for (const FieldDescriptor* f : fields) {
    if (!keep.count(f)) continue;

    if (f->is_repeated()) {
      int n = ref->FieldSize(src, f);
      for (int i = 0; i < n; ++i) {
        switch (f->cpp_type()) {
          case FieldDescriptor::CPPTYPE_INT32:   ref->AddInt32 (&dst, f, ref->GetRepeatedInt32 (src, f, i)); break;
          case FieldDescriptor::CPPTYPE_INT64:   ref->AddInt64 (&dst, f, ref->GetRepeatedInt64 (src, f, i)); break;
          case FieldDescriptor::CPPTYPE_UINT32:  ref->AddUInt32(&dst, f, ref->GetRepeatedUInt32(src, f, i)); break;
          case FieldDescriptor::CPPTYPE_UINT64:  ref->AddUInt64(&dst, f, ref->GetRepeatedUInt64(src, f, i)); break;
          case FieldDescriptor::CPPTYPE_BOOL:    ref->AddBool  (&dst, f, ref->GetRepeatedBool  (src, f, i)); break;
          case FieldDescriptor::CPPTYPE_FLOAT:   ref->AddFloat (&dst, f, ref->GetRepeatedFloat (src, f, i)); break;
          case FieldDescriptor::CPPTYPE_DOUBLE:  ref->AddDouble(&dst, f, ref->GetRepeatedDouble(src, f, i)); break;
          case FieldDescriptor::CPPTYPE_STRING:  ref->AddString(&dst, f, ref->GetRepeatedString(src, f, i)); break;
          case FieldDescriptor::CPPTYPE_ENUM:    ref->AddEnum  (&dst, f, ref->GetRepeatedEnum  (src, f, i)); break;
          case FieldDescriptor::CPPTYPE_MESSAGE: {
            Message* sub = ref->AddMessage(&dst, f);
            sub->CopyFrom(ref->GetRepeatedMessage(src, f, i));
            break;
          }
        }
      }
    } else {
      if (!ref->HasField(src, f)) continue;
      switch (f->cpp_type()) {
        case FieldDescriptor::CPPTYPE_INT32:   ref->SetInt32 (&dst, f, ref->GetInt32 (src, f)); break;
        case FieldDescriptor::CPPTYPE_INT64:   ref->SetInt64 (&dst, f, ref->GetInt64 (src, f)); break;
        case FieldDescriptor::CPPTYPE_UINT32:  ref->SetUInt32(&dst, f, ref->GetUInt32(src, f)); break;
        case FieldDescriptor::CPPTYPE_UINT64:  ref->SetUInt64(&dst, f, ref->GetUInt64(src, f)); break;
        case FieldDescriptor::CPPTYPE_BOOL:    ref->SetBool  (&dst, f, ref->GetBool  (src, f)); break;
        case FieldDescriptor::CPPTYPE_FLOAT:   ref->SetFloat (&dst, f, ref->GetFloat (src, f)); break;
        case FieldDescriptor::CPPTYPE_DOUBLE:  ref->SetDouble(&dst, f, ref->GetDouble(src, f)); break;
        case FieldDescriptor::CPPTYPE_STRING:  ref->SetString(&dst, f, ref->GetString(src, f)); break;
        case FieldDescriptor::CPPTYPE_ENUM:    ref->SetEnum  (&dst, f, ref->GetEnum  (src, f)); break;
        case FieldDescriptor::CPPTYPE_MESSAGE: {
          Message* sub = ref->MutableMessage(&dst, f);
          sub->CopyFrom(ref->GetMessage(src, f));
          break;
        }
      }
    }
  }
}

int main(int argc, char** argv) {
  Args args;
  if (!parseArgs(argc, argv, args)) {
    usage(argv[0]);
    return 1;
  }

  // Load descriptor set and build pool
  DescriptorPool pool; // will be bound to our global built pool in loadPool
  std::unique_ptr<FileDescriptorSet> fds_backing;
  if (!loadPool(args.desc_file, pool, fds_backing)) return 2;

  const Descriptor* desc = pool.FindMessageTypeByName(args.type_name);
  if (!desc) {
    std::cerr << "Type not found in descriptor set: " << args.type_name << "\n";
    return 3;
  }

  DynamicMessageFactory factory;
  std::unique_ptr<Message> prototype(factory.GetPrototype(desc)->New());
  if (!prototype) {
    std::cerr << "Failed to create prototype for " << args.type_name << "\n";
    return 4;
  }

  // Prepare field lists
  std::unordered_set<std::string> key_names(args.key_fields.begin(), args.key_fields.end());
  std::vector<const FieldDescriptor*> nonkey_fields;
  std::vector<const FieldDescriptor*> key_fields;
  for (int i = 0; i < desc->field_count(); ++i) {
    const FieldDescriptor* f = desc->field(i);
    if (key_names.count(f->name())) key_fields.push_back(f);
    else nonkey_fields.push_back(f);
  }
  // Split non-keys roughly in half
  size_t half = nonkey_fields.size() / 2;
  std::vector<const FieldDescriptor*> groupA_fields(key_fields.begin(), key_fields.end());
  std::vector<const FieldDescriptor*> groupB_fields(key_fields.begin(), key_fields.end());
  groupA_fields.insert(groupA_fields.end(), nonkey_fields.begin(), nonkey_fields.begin() + half);
  groupB_fields.insert(groupB_fields.end(), nonkey_fields.begin() + half, nonkey_fields.end());

  std::unordered_set<const FieldDescriptor*> keepA(groupA_fields.begin(), groupA_fields.end());
  std::unordered_set<const FieldDescriptor*> keepB(groupB_fields.begin(), groupB_fields.end());

  // Open I/O
  std::ifstream fin(args.input_file, std::ios::binary);
  if (!fin) { std::cerr << "Failed to open input: " << args.input_file << "\n"; return 5; }

  std::string outA = args.out_dir + "/groupA.binpb";
  std::string outB = args.out_dir + "/groupB.binpb";
  std::ofstream foutA(outA, std::ios::binary);
  std::ofstream foutB(outB, std::ios::binary);
  if (!foutA || !foutB) { std::cerr << "Failed to open outputs in: " << args.out_dir << "\n"; return 6; }

  io::IstreamInputStream zin(&fin);
  io::OstreamOutputStream zoutA(&foutA);
  io::OstreamOutputStream zoutB(&foutB);

  uint64_t count = 0;
  while (true) {
    std::unique_ptr<Message> msg(prototype->New());
    bool clean_eof = false;
    if (!google::protobuf::util::ParseDelimitedFromZeroCopyStream(msg.get(), &zin, &clean_eof)) {
      if (clean_eof) break;
      std::cerr << "Parse error at record #" << count << "\n";
      return 7;
    }
    // Build A and B
    std::unique_ptr<Message> msgA(prototype->New());
    std::unique_ptr<Message> msgB(prototype->New());
    copySubset(*msg, *msgA, keepA);
    copySubset(*msg, *msgB, keepB);

    if (!google::protobuf::util::SerializeDelimitedToZeroCopyStream(*msgA, &zoutA)) {
      std::cerr << "Serialize A failed at #" << count << "\n";
      return 8;
    }
    if (!google::protobuf::util::SerializeDelimitedToZeroCopyStream(*msgB, &zoutB)) {
      std::cerr << "Serialize B failed at #" << count << "\n";
      return 9;
    }
    ++count;
  }

  std::cerr << "Split complete. Records processed: " << count << "\n";
  return 0;
}