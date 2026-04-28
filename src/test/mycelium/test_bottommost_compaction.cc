// Copyright (c) 2024-present, Mycelium Authors.
//
// Integration test: bottommost compaction source-xor-dest invariant (Track B #5).
//
// Claim: after a bottommost compaction with an admitting policy, every key
// written to the source CF has moved to the dest CF and is no longer readable
// from the source CF.  When the policy blocks admission, the key stays in the
// source CF — even at the bottommost level.
//
// Tests:
//   1. AlwaysAdmit_ConvertsAtBottommost  — key leaves source, arrives in dest
//   2. NeverAdmit_KeyStaysInSource       — key stays in source, absent from dest
//   3. DeferThenAdmit_NaturalCatchup     — key is deferred, then caught up when
//                                          policy is relaxed and data is
//                                          re-compacted at the bottommost level

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "rocksdb/db.h"
#include "rocksdb/mym_broker.h"
#include "rocksdb/options.h"
#include "mycelium/admission_policy.h"
#include "mycelium/json_encoder.h"
#include "mycelium/json_parser.h"
#include "mycelium/mynooper.h"
#include "mycelium/transformer.h"

#include "gtest/gtest.h"

using ROCKSDB_NAMESPACE::BottommostLevelCompaction;
using ROCKSDB_NAMESPACE::CompactRangeOptions;
using ROCKSDB_NAMESPACE::FlushOptions;
using ROCKSDB_NAMESPACE::MymBroker;
using ROCKSDB_NAMESPACE::ReadOptions;
using ROCKSDB_NAMESPACE::Status;

// ── ToggleAdmissionPolicy ────────────────────────────────────────────────────
// A minimal policy whose admit-decision can be flipped at runtime via an
// atomic boolean.  Starts as "never admit" (admit_ = false).

class ToggleAdmissionPolicy : public mycelium::AdmissionPolicy {
 public:
  explicit ToggleAdmissionPolicy(bool initial = false) : admit_(initial) {}

  void SetAdmit(bool v) { admit_.store(v, std::memory_order_release); }

  bool ShouldAdmit(double /*cpu*/, int /*pending*/,
                   uint64_t /*bytes*/) const override {
    return admit_.load(std::memory_order_acquire);
  }

  std::string Name() const override { return "ToggleAdmissionPolicy"; }

 private:
  std::atomic<bool> admit_;
};

// ── Test fixture ─────────────────────────────────────────────────────────────

class BottommostCompactionTest : public ::testing::Test {
 protected:
  static std::string TmpDir(const std::string& suffix) {
    return "/tmp/mycelium_bmc_" + suffix;
  }

  // Build Options wired to a Mynooper (IDENTITY) transformer.
  // The dest CF is named <root>_identity_cf.
  // admission_policy: nullptr => always admit; otherwise use provided policy.
  static ROCKSDB_NAMESPACE::Options MakeOptions(
      std::shared_ptr<mycelium::AdmissionPolicy> policy = nullptr) {
    ROCKSDB_NAMESPACE::Options opts;
    opts.create_if_missing        = true;
    opts.error_if_exists          = false;
    opts.disable_auto_compactions = true;   // manual compaction control
    opts.num_levels               = 3;
    opts.num_columns              = 1;
    opts.compaction_style = ROCKSDB_NAMESPACE::kCompactionStyleLevel;
    opts.info_log_level   = ROCKSDB_NAMESPACE::InfoLogLevel::WARN_LEVEL;

    opts.transformers.push_back(std::make_shared<mycelium::Mynooper>());
    opts.admission_policy = std::move(policy);

    // Minimal 1-column JSON schema — values look like {"col0":"v<n>"}
    const int kCols = 1;
    auto parser = std::make_shared<mycelium::JsonColsParser>(kCols, 0);
    auto enc    = std::make_shared<mycelium::JsonEncoder>();
    mycelium::Codec in_codec{parser, nullptr};
    mycelium::Codec out_codec{nullptr, enc};
    std::vector<mycelium::FieldSchema> in_schema = parser->GetInputFieldSchema();
    std::vector<std::vector<mycelium::FieldSchema>> out_schemas;
    out_schemas.push_back(in_schema);
    opts.schemaDescriptors.push_back(std::make_shared<mycelium::SchemaDescriptor>(
        in_codec, out_codec, std::move(in_schema), std::move(out_schemas)));

    return opts;
  }

  // Write n keys of the form prefix+i → {"col0":"v<i>"} into the broker.
  static std::vector<std::string> WriteKeys(MymBroker& broker, int n,
                                             const std::string& prefix = "key") {
    std::vector<std::string> keys;
    keys.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) {
      std::string key = prefix + std::to_string(i);
      std::string val = "{\"col0\":\"v" + std::to_string(i) + "\"}";
      EXPECT_EQ(broker.Insert(key, val), 0) << "Insert failed for key " << key;
      keys.push_back(std::move(key));
    }
    return keys;
  }

  // Flush the source CF to L0 then force a full bottommost compaction.
  static void FlushAndCompact(ROCKSDB_NAMESPACE::DB* db,
                               ROCKSDB_NAMESPACE::ColumnFamilyHandle* src_cf) {
    FlushOptions fo;
    fo.wait = true;
    ASSERT_TRUE(db->Flush(fo, src_cf).ok()) << "Flush failed";

    CompactRangeOptions cro;
    cro.bottommost_level_compaction = BottommostLevelCompaction::kForce;
    cro.change_level = false;
    auto s = db->CompactRange(cro, src_cf, nullptr, nullptr);
    EXPECT_TRUE(s.ok()) << "CompactRange failed: " << s.ToString();
    if(!s.ok()) { std::cout << "Compaction error: " << s.ToString() << std::endl; }
  }
};

// ============================================================================
// 1. AlwaysAdmit — key leaves source CF, arrives in dest CF
// ============================================================================

TEST_F(BottommostCompactionTest, AlwaysAdmit_ConvertsAtBottommost) {
  const std::string kRoot = "myc_test";
  const std::string kDB   = TmpDir("always_admit");
  std::filesystem::remove_all(kDB);

  // nullptr admission_policy => AlwaysAdmit (original behaviour)
  auto opts = MakeOptions(/*policy=*/nullptr);
  MymBroker broker(kRoot, /*cf_created=*/false, kDB.c_str(), opts, /*splits=*/1);

  const auto keys = WriteKeys(broker, 5);

  auto* db      = broker.GetDB();
  auto* src_cf  = broker.GetCFHandle(kRoot);
  auto* dest_cf = broker.GetCFHandle(kRoot + "_identity_cf");
  ASSERT_NE(db,      nullptr) << "GetDB() returned null";
  ASSERT_NE(src_cf,  nullptr) << "source CF handle not found";
  ASSERT_NE(dest_cf, nullptr) << "dest CF handle not found";

  FlushAndCompact(db, src_cf);

  
    std::string num_files;
    db->GetProperty(dest_cf, "rocksdb.num-files-at-level0", &num_files);
    std::cout << "dest_cf L0 files: " << num_files << std::endl;

  for (const auto& key : keys) {
    std::string val;
    EXPECT_TRUE(db->Get(ReadOptions(), src_cf, key, &val).IsNotFound())
        << "Key '" << key << "' should have left source CF after transform";
    EXPECT_TRUE(db->Get(ReadOptions(), dest_cf, key, &val).ok())
        << "Key '" << key << "' should be in dest CF after transform";
  }

  std::filesystem::remove_all(kDB);
}

// ============================================================================
// 2. NeverAdmit — key stays in source CF even at the bottommost level
// ============================================================================

TEST_F(BottommostCompactionTest, NeverAdmit_KeyStaysInSource) {
  const std::string kRoot = "myc_test";
  const std::string kDB   = TmpDir("never_admit");
  std::filesystem::remove_all(kDB);

  auto opts = MakeOptions(std::make_shared<mycelium::NeverAdmitPolicy>());
  MymBroker broker(kRoot, false, kDB.c_str(), opts, 1);

  const auto keys = WriteKeys(broker, 5);

  auto* db      = broker.GetDB();
  auto* src_cf  = broker.GetCFHandle(kRoot);
  auto* dest_cf = broker.GetCFHandle(kRoot + "_identity_cf");
  ASSERT_NE(db,      nullptr);
  ASSERT_NE(src_cf,  nullptr);
  ASSERT_NE(dest_cf, nullptr);

  FlushAndCompact(db, src_cf);

  
    std::string num_files;
    db->GetProperty(dest_cf, "rocksdb.num-files-at-level0", &num_files);
    std::cout << "dest_cf L0 files: " << num_files << std::endl;

  for (const auto& key : keys) {
    std::string val;
    EXPECT_TRUE(db->Get(ReadOptions(), src_cf, key, &val).ok())
        << "Key '" << key << "' should remain in source CF (NeverAdmit)";
    EXPECT_TRUE(db->Get(ReadOptions(), dest_cf, key, &val).IsNotFound())
        << "Key '" << key << "' should not be in dest CF (NeverAdmit)";
  }

  std::filesystem::remove_all(kDB);
}

// ============================================================================
// 3. DeferThenAdmit — natural catch-up when policy is relaxed
//
// Phase 1: compact with policy OFF (= defer) → keys stay in source CF.
// Phase 2: relax policy, force re-compaction at bottommost level → catch-up
//          fires and keys move to dest CF.
// ============================================================================

TEST_F(BottommostCompactionTest, DeferThenAdmit_NaturalCatchup) {
  const std::string kRoot = "myc_test";
  const std::string kDB   = TmpDir("defer_then_admit");
  std::filesystem::remove_all(kDB);

  auto toggle = std::make_shared<ToggleAdmissionPolicy>(/*initial=*/false);
  auto opts   = MakeOptions(toggle);
  MymBroker broker(kRoot, false, kDB.c_str(), opts, 1);

  const auto keys = WriteKeys(broker, 5);

  auto* db      = broker.GetDB();
  auto* src_cf  = broker.GetCFHandle(kRoot);
  auto* dest_cf = broker.GetCFHandle(kRoot + "_identity_cf");
  ASSERT_NE(db,      nullptr);
  ASSERT_NE(src_cf,  nullptr);
  ASSERT_NE(dest_cf, nullptr);

  // ── Phase 1: policy OFF → deferred ────────────────────────────────────────
  FlushAndCompact(db, src_cf);

  
    std::string num_files;
    db->GetProperty(dest_cf, "rocksdb.num-files-at-level0", &num_files);
    std::cout << "dest_cf L0 files: " << num_files << std::endl;

  for (const auto& key : keys) {
    std::string val;
    EXPECT_TRUE(db->Get(ReadOptions(), src_cf, key, &val).ok())
        << "Phase 1: key '" << key << "' should still be in source CF (deferred)";
    EXPECT_TRUE(db->Get(ReadOptions(), dest_cf, key, &val).IsNotFound())
        << "Phase 1: key '" << key << "' should not yet be in dest CF";
  }

  // ── Phase 2: relax policy, re-compact bottommost → catch-up ──────────────
  toggle->SetAdmit(true);

  CompactRangeOptions cro;
  cro.bottommost_level_compaction = BottommostLevelCompaction::kForce;
  ASSERT_TRUE(db->CompactRange(cro, src_cf, nullptr, nullptr).ok())
      << "Phase 2 CompactRange failed";

  
    std::string num_files;
    db->GetProperty(dest_cf, "rocksdb.num-files-at-level0", &num_files);
    std::cout << "dest_cf L0 files: " << num_files << std::endl;

  for (const auto& key : keys) {
    std::string val;
    EXPECT_TRUE(db->Get(ReadOptions(), src_cf, key, &val).IsNotFound())
        << "Phase 2: key '" << key << "' should have left source CF";
    EXPECT_TRUE(db->Get(ReadOptions(), dest_cf, key, &val).ok())
        << "Phase 2: key '" << key << "' should now be in dest CF";
  }

  std::filesystem::remove_all(kDB);
}

// ── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
