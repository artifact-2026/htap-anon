// Copyright (c) 2024-present, Mycelium Authors.
//
// Integration test: level-eligibility once-only correctness gate (E16).
//
// Claim: the SPLIT (Distributor) transformer fires exactly once per
// (source SST, dest CF) pair across all compaction passes, regardless of
// how many compaction jobs subsequently touch that SST.  The guarantee is
// enforced by TransformEpochTracker, whose encoded state is stored in each
// SST's user-collected table properties under the key "mycelium.epoch".
//
// After Holly's clarification (2026-04-27):
//   When source CF SPLIT-tags key1→{col0,col1,col2,col3}, the compaction
//   job routes output to destCF1:{col0,col1} and destCF2:{col2,col3}.
//   If destCF1/destCF2 are themselves SPLIT-tagged, their OWN subsequent
//   compaction jobs further split them; the source SST is never revisited.
//   Each split fires exactly once on ITS own source SST and writes to
//   fresh dest CFs — no reentrance.
//
// Tests:
//   1. OnceFiring_EpochAppliedAfterCompaction
//        SPLIT fires → epoch property shows APPLIED for both dest CFs.
//   2. NoReFiring_EpochPreventsDoubleApplication
//        A second force-compact on the SAME output SSTs must not fire the
//        transform again; dest CF record counts must not double.
//   3. DataIntegrity_CorrectColumnsInDestCFs
//        After compaction, destCF1 holds {col0,col1} and destCF2 holds
//        {col2,col3}; no key appears in the wrong CF or more than once.
//   4. RecursiveSplit_TwoLevelDataIntegrity
//        Source → destCF1{col0,col1} + destCF2{col2,col3}.
//        destCF1 → destCF21{col0} + destCF22{col1}.
//        destCF2 → destCF23{col2} + destCF24{col3}.
//        After compaction of all levels: each leaf CF holds exactly one
//        column; no record appears twice anywhere.

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "rocksdb/db.h"
#include "rocksdb/mym_broker.h"
#include "rocksdb/options.h"
#include "mycelium/admission_policy.h"
#include "mycelium/distributor.h"
#include "mycelium/json_encoder.h"
#include "mycelium/json_parser.h"
#include "mycelium/transform_epoch_tracker.h"

// The SST property key written by EpochIntTblPropCollector.
#include "db/mycelium_adapter/epoch_table_properties_collector.h"

#include "gtest/gtest.h"

using ROCKSDB_NAMESPACE::BottommostLevelCompaction;
using ROCKSDB_NAMESPACE::CompactRangeOptions;
using ROCKSDB_NAMESPACE::FlushOptions;
using ROCKSDB_NAMESPACE::MymBroker;
using ROCKSDB_NAMESPACE::ReadOptions;
using ROCKSDB_NAMESPACE::TablePropertiesCollection;
using mycelium::TransformEpochTracker;
using mycelium::TransformState;

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::string TmpDir(const std::string& suffix) {
  return "/tmp/mycelium_e16_" + suffix;
}

// Build a 4-column JSON value: {"col0":"v0","col1":"v1","col2":"v2","col3":"v3"}
static std::string Make4ColValue(int i) {
  return "{\"col0\":\"v" + std::to_string(i) + "_0\","
         "\"col1\":\"v" + std::to_string(i) + "_1\","
         "\"col2\":\"v" + std::to_string(i) + "_2\","
         "\"col3\":\"v" + std::to_string(i) + "_3\"}";
}

// Write n records into the broker and return the key list.
static std::vector<std::string> WriteN(MymBroker& broker, int n,
                                       const std::string& prefix = "key") {
  std::vector<std::string> keys;
  keys.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; i++) {
    std::string key = prefix + std::to_string(i);
    EXPECT_EQ(broker.Insert(key, Make4ColValue(i)), 0)
        << "Insert failed for " << key;
    keys.push_back(key);
  }
  return keys;
}

// Force a full bottommost compaction on cf.
static void ForceBottommost(ROCKSDB_NAMESPACE::DB* db,
                             ROCKSDB_NAMESPACE::ColumnFamilyHandle* cf) {
  FlushOptions fo;
  fo.wait = true;
  ASSERT_TRUE(db->Flush(fo, cf).ok());

  CompactRangeOptions cro;
  cro.bottommost_level_compaction = BottommostLevelCompaction::kForce;
  cro.change_level = false;
  ASSERT_TRUE(db->CompactRange(cro, cf, nullptr, nullptr).ok());
}

// Read the "mycelium.epoch" property from every SST in cf_handle and return
// the decoded TransformEpochTracker per SST path.
static std::unordered_map<std::string, TransformEpochTracker>
ReadEpochsForCF(ROCKSDB_NAMESPACE::DB* db,
                ROCKSDB_NAMESPACE::ColumnFamilyHandle* cf_handle) {
  std::unordered_map<std::string, TransformEpochTracker> result;
  TablePropertiesCollection props;
  EXPECT_TRUE(db->GetPropertiesOfAllTables(cf_handle, &props).ok());

  for (const auto& [sst_path, tp] : props) {
    auto it = tp->user_collected_properties.find(
        ROCKSDB_NAMESPACE::kEpochPropertyKey);
    if (it == tp->user_collected_properties.end()) continue;

    TransformEpochTracker tracker;
    auto s = tracker.DecodeFrom(it->second);
    EXPECT_TRUE(s.ok()) << "Bad epoch encoding in " << sst_path
                        << ": " << s.message();
    result[sst_path] = std::move(tracker);
  }
  return result;
}

// Count all unique keys in cf_handle via a full scan.
static int CountKeysInCF(ROCKSDB_NAMESPACE::DB* db,
                          ROCKSDB_NAMESPACE::ColumnFamilyHandle* cf_handle) {
  auto* it = db->NewIterator(ReadOptions(), cf_handle);
  int count = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) count++;
  EXPECT_TRUE(it->status().ok());
  delete it;
  return count;
}

// ── Build a 4-column SPLIT broker: source → destCF1{col0,col1} + destCF2{col2,col3}
static ROCKSDB_NAMESPACE::Options Make4ColSplitOptions() {
  ROCKSDB_NAMESPACE::Options opts;
  opts.create_if_missing        = true;
  opts.error_if_exists          = false;
  opts.disable_auto_compactions = true;
  opts.num_levels               = 4;
  opts.compaction_style = ROCKSDB_NAMESPACE::kCompactionStyleLevel;
  opts.info_log_level   = ROCKSDB_NAMESPACE::InfoLogLevel::FATAL_LEVEL;

  // SPLIT: col0+col1 → destCF1, col2+col3 → destCF2
  mycelium::SplitByPositions splits = {{0, 1}, {2, 3}};
  opts.transformers.push_back(
      std::make_shared<mycelium::Distributor>(splits));

  const int kCols = 4;
  auto parser = std::make_shared<mycelium::JsonColsParser>(kCols, 0);
  auto enc    = std::make_shared<mycelium::JsonEncoder>();
  mycelium::Codec in_codec{parser, nullptr};
  mycelium::Codec out_codec{nullptr, enc};

  std::vector<mycelium::FieldSchema> in_schema;
  for (int i = 0; i < kCols; i++)
    in_schema.push_back({"col" + std::to_string(i), "string", i});

  // out_schemas: two groups
  std::vector<mycelium::FieldSchema> out1 = {{"col0", "string", 0},
                                              {"col1", "string", 1}};
  std::vector<mycelium::FieldSchema> out2 = {{"col2", "string", 2},
                                              {"col3", "string", 3}};
  opts.schemaDescriptors.push_back(std::make_shared<mycelium::SchemaDescriptor>(
      in_codec, out_codec, in_schema,
      std::vector<std::vector<mycelium::FieldSchema>>{out1, out2}));

  return opts;
}

// ── Test fixture ─────────────────────────────────────────────────────────────

class LevelEligibilityTest : public ::testing::Test {
 protected:
  void TearDown() override {
    if (!db_path_.empty()) std::filesystem::remove_all(db_path_);
  }
  std::string db_path_;
};

// ============================================================================
// 1. OnceFiring_EpochAppliedAfterCompaction
//
// After the first bottommost compaction, every output SST in the source CF
// must have a "mycelium.epoch" property with APPLIED state for both dest CFs.
// ============================================================================

TEST_F(LevelEligibilityTest, OnceFiring_EpochAppliedAfterCompaction) {
  db_path_ = TmpDir("once_firing");
  std::filesystem::remove_all(db_path_);

  const std::string kRoot = "myc_split";
  auto opts = Make4ColSplitOptions();
  MymBroker broker(kRoot, /*cf_created=*/false, db_path_.c_str(), opts,
                   /*splits=*/2);

  WriteN(broker, /*n=*/20);

  auto* db      = broker.GetDB();
  auto* src_cf  = broker.GetCFHandle(kRoot);
  ASSERT_NE(db,     nullptr);
  ASSERT_NE(src_cf, nullptr);

  ForceBottommost(db, src_cf);

  // Read epoch properties from every output SST in the source CF.
  auto epochs = ReadEpochsForCF(db, src_cf);
  ASSERT_FALSE(epochs.empty())
      << "No SSTs found in source CF after compaction";

  const std::string destCF1 = kRoot + "_split0_cf";
  const std::string destCF2 = kRoot + "_split1_cf";

  for (const auto& [sst_path, tracker] : epochs) {
    EXPECT_EQ(tracker.GetState(destCF1), TransformState::APPLIED)
        << "SST " << sst_path << ": expected APPLIED for " << destCF1;
    EXPECT_EQ(tracker.GetState(destCF2), TransformState::APPLIED)
        << "SST " << sst_path << ": expected APPLIED for " << destCF2;
    // No spurious entries for CFs that don't exist in this topology.
    auto applied = tracker.GetAppliedCFs();
    EXPECT_EQ(applied.size(), 2u)
        << "SST " << sst_path << ": expected exactly 2 APPLIED CFs";
  }
}

// ============================================================================
// 2. NoReFiring_EpochPreventsDoubleApplication
//
// A second force-compact on the post-transform SSTs must not re-apply the
// transform.  Dest CF record counts must remain equal to the number of
// written keys — not double.
// ============================================================================

TEST_F(LevelEligibilityTest, NoReFiring_EpochPreventsDoubleApplication) {
  db_path_ = TmpDir("no_refire");
  std::filesystem::remove_all(db_path_);

  const std::string kRoot = "myc_split";
  auto opts = Make4ColSplitOptions();
  MymBroker broker(kRoot, false, db_path_.c_str(), opts, 2);

  const int kN = 20;
  WriteN(broker, kN);

  auto* db     = broker.GetDB();
  auto* src_cf = broker.GetCFHandle(kRoot);
  const std::string destCF1 = kRoot + "_split0_cf";
  const std::string destCF2 = kRoot + "_split1_cf";
  auto* cf1    = broker.GetCFHandle(destCF1);
  auto* cf2    = broker.GetCFHandle(destCF2);
  ASSERT_NE(db,  nullptr);
  ASSERT_NE(src_cf, nullptr);
  ASSERT_NE(cf1, nullptr);
  ASSERT_NE(cf2, nullptr);

  // First compaction — transforms fire.
  ForceBottommost(db, src_cf);
  const int after_first_cf1 = CountKeysInCF(db, cf1);
  const int after_first_cf2 = CountKeysInCF(db, cf2);
  EXPECT_EQ(after_first_cf1, kN)
      << "destCF1 should have exactly kN records after first compaction";
  EXPECT_EQ(after_first_cf2, kN)
      << "destCF2 should have exactly kN records after first compaction";

  // Second force-compact — epoch tracker must prevent re-application.
  ForceBottommost(db, src_cf);
  const int after_second_cf1 = CountKeysInCF(db, cf1);
  const int after_second_cf2 = CountKeysInCF(db, cf2);
  EXPECT_EQ(after_second_cf1, kN)
      << "destCF1 count must not change after second compaction (no re-fire)";
  EXPECT_EQ(after_second_cf2, kN)
      << "destCF2 count must not change after second compaction (no re-fire)";

  // Epoch state must still be APPLIED (not reset or re-deferred).
  auto epochs = ReadEpochsForCF(db, src_cf);
  for (const auto& [sst_path, tracker] : epochs) {
    EXPECT_EQ(tracker.GetState(destCF1), TransformState::APPLIED)
        << "SST " << sst_path << ": epoch must stay APPLIED after re-compact";
    EXPECT_EQ(tracker.GetState(destCF2), TransformState::APPLIED)
        << "SST " << sst_path << ": epoch must stay APPLIED after re-compact";
  }
}

// ============================================================================
// 3. DataIntegrity_CorrectColumnsInDestCFs
//
// After compaction, destCF1 contains records with {col0, col1} and destCF2
// with {col2, col3}.  Each key appears exactly once in each dest CF.
// The source CF retains the original full-column records (key-passthrough).
// ============================================================================

TEST_F(LevelEligibilityTest, DataIntegrity_CorrectColumnsInDestCFs) {
  db_path_ = TmpDir("data_integrity");
  std::filesystem::remove_all(db_path_);

  const std::string kRoot = "myc_split";
  auto opts = Make4ColSplitOptions();
  MymBroker broker(kRoot, false, db_path_.c_str(), opts, 2);

  const int kN = 10;
  auto keys = WriteN(broker, kN);

  auto* db     = broker.GetDB();
  auto* src_cf = broker.GetCFHandle(kRoot);
  auto* cf1    = broker.GetCFHandle(kRoot + "_split0_cf");
  auto* cf2    = broker.GetCFHandle(kRoot + "_split1_cf");
  ASSERT_NE(db,     nullptr);
  ASSERT_NE(src_cf, nullptr);
  ASSERT_NE(cf1,    nullptr);
  ASSERT_NE(cf2,    nullptr);

  ForceBottommost(db, src_cf);

  // Every written key must be present in both dest CFs exactly once.
  std::set<std::string> seen_cf1, seen_cf2;
  auto* it1 = db->NewIterator(ReadOptions(), cf1);
  for (it1->SeekToFirst(); it1->Valid(); it1->Next()) {
    const std::string k = it1->key().ToString();
    EXPECT_TRUE(seen_cf1.insert(k).second)
        << "Duplicate key '" << k << "' in destCF1";
    // Value must only contain col0 and col1 (not col2 or col3).
    const std::string val = it1->value().ToString();
    EXPECT_NE(val.find("col0"), std::string::npos)
        << "destCF1 record missing col0 for key " << k;
    EXPECT_NE(val.find("col1"), std::string::npos)
        << "destCF1 record missing col1 for key " << k;
    EXPECT_EQ(val.find("col2"), std::string::npos)
        << "destCF1 record should NOT contain col2 for key " << k;
    EXPECT_EQ(val.find("col3"), std::string::npos)
        << "destCF1 record should NOT contain col3 for key " << k;
  }
  EXPECT_TRUE(it1->status().ok());
  delete it1;

  auto* it2 = db->NewIterator(ReadOptions(), cf2);
  for (it2->SeekToFirst(); it2->Valid(); it2->Next()) {
    const std::string k = it2->key().ToString();
    EXPECT_TRUE(seen_cf2.insert(k).second)
        << "Duplicate key '" << k << "' in destCF2";
    const std::string val = it2->value().ToString();
    EXPECT_EQ(val.find("col0"), std::string::npos)
        << "destCF2 record should NOT contain col0 for key " << k;
    EXPECT_EQ(val.find("col1"), std::string::npos)
        << "destCF2 record should NOT contain col1 for key " << k;
    EXPECT_NE(val.find("col2"), std::string::npos)
        << "destCF2 record missing col2 for key " << k;
    EXPECT_NE(val.find("col3"), std::string::npos)
        << "destCF2 record missing col3 for key " << k;
  }
  EXPECT_TRUE(it2->status().ok());
  delete it2;

  // All written keys must appear in both dest CFs.
  EXPECT_EQ(static_cast<int>(seen_cf1.size()), kN)
      << "destCF1 key count mismatch";
  EXPECT_EQ(static_cast<int>(seen_cf2.size()), kN)
      << "destCF2 key count mismatch";
  for (const auto& k : keys) {
    EXPECT_TRUE(seen_cf1.count(k))  << "Key '" << k << "' missing from destCF1";
    EXPECT_TRUE(seen_cf2.count(k))  << "Key '" << k << "' missing from destCF2";
  }
}

// ============================================================================
// 4. RecursiveSplit_TwoLevelDataIntegrity
//
// Two-level SPLIT grove:
//   source  → destCF1{col0,col1} + destCF2{col2,col3}   (first compaction)
//   destCF1 → destCF21{col0}     + destCF22{col1}        (second compaction)
//   destCF2 → destCF23{col2}     + destCF24{col3}        (third compaction)
//
// Per Holly's clarification: the source SST is NOT revisited when destCF1
// and destCF2 are compacted; each split fires on its OWN source SST.
// After all compactions:
//   - destCF21 has exactly col0 for every key.
//   - destCF22 has exactly col1 for every key.
//   - No key appears twice in any leaf CF.
//   - Source SST epoch still shows APPLIED for destCF1 and destCF2 only
//     (not for destCF21/22/23/24 — those belong to destCF1/2's epochs).
//
// Note: this test uses MymBroker to construct the second-level grove for
// destCF1 and destCF2, opening separate broker instances over the same DB
// directory with destCFx as the root.
// ============================================================================

TEST_F(LevelEligibilityTest, RecursiveSplit_TwoLevelDataIntegrity) {
  db_path_ = TmpDir("recursive_split");
  std::filesystem::remove_all(db_path_);

  // ── Level 1: source → destCF1{col0,col1} + destCF2{col2,col3} ────────────
  const std::string kRoot = "myc_split";
  auto opts_l1 = Make4ColSplitOptions();
  MymBroker broker_l1(kRoot, false, db_path_.c_str(), opts_l1, 2);

  const int kN  = 10;
  auto keys = WriteN(broker_l1, kN);

  auto* db     = broker_l1.GetDB();
  auto* src_cf = broker_l1.GetCFHandle(kRoot);
  const std::string destCF1_name = kRoot + "_split0_cf";
  const std::string destCF2_name = kRoot + "_split1_cf";
  auto* destCF1 = broker_l1.GetCFHandle(destCF1_name);
  auto* destCF2 = broker_l1.GetCFHandle(destCF2_name);
  ASSERT_NE(db,      nullptr);
  ASSERT_NE(src_cf,  nullptr);
  ASSERT_NE(destCF1, nullptr);
  ASSERT_NE(destCF2, nullptr);

  ForceBottommost(db, src_cf);

  // Source SST epoch: only destCF1 and destCF2 are APPLIED.
  {
    auto epochs = ReadEpochsForCF(db, src_cf);
    for (const auto& [sst_path, tracker] : epochs) {
      EXPECT_EQ(tracker.GetState(destCF1_name), TransformState::APPLIED)
          << sst_path;
      EXPECT_EQ(tracker.GetState(destCF2_name), TransformState::APPLIED)
          << sst_path;
      // destCF21/22/23/24 must NOT appear in source SST's epoch.
      EXPECT_EQ(tracker.GetState(destCF1_name + "_split0_cf"),
                TransformState::UNKNOWN)
          << "Source SST epoch must not reference grandchild CFs";
      EXPECT_EQ(tracker.GetState(destCF1_name + "_split1_cf"),
                TransformState::UNKNOWN)
          << "Source SST epoch must not reference grandchild CFs";
    }
  }

  // ── Level 2a: destCF1{col0,col1} → destCF21{col0} + destCF22{col1} ───────
  // Configure a 2-column SPLIT rooted at destCF1.
  ROCKSDB_NAMESPACE::Options opts_l2a;
  opts_l2a.create_if_missing        = false;  // DB already exists
  opts_l2a.disable_auto_compactions = true;
  opts_l2a.num_levels               = 4;
  opts_l2a.compaction_style = ROCKSDB_NAMESPACE::kCompactionStyleLevel;
  opts_l2a.info_log_level   = ROCKSDB_NAMESPACE::InfoLogLevel::FATAL_LEVEL;
  {
    mycelium::SplitByPositions splits2 = {{0}, {1}};
    opts_l2a.transformers.push_back(
        std::make_shared<mycelium::Distributor>(splits2));
    auto parser2 = std::make_shared<mycelium::JsonColsParser>(2, 0);
    auto enc2    = std::make_shared<mycelium::JsonEncoder>();
    mycelium::Codec in2{parser2, nullptr};
    mycelium::Codec out2{nullptr, enc2};
    std::vector<mycelium::FieldSchema> in_s2 = {{"col0", "string", 0},
                                                 {"col1", "string", 1}};
    std::vector<mycelium::FieldSchema> o21   = {{"col0", "string", 0}};
    std::vector<mycelium::FieldSchema> o22   = {{"col1", "string", 1}};
    opts_l2a.schemaDescriptors.push_back(
        std::make_shared<mycelium::SchemaDescriptor>(
            in2, out2, in_s2,
            std::vector<std::vector<mycelium::FieldSchema>>{o21, o22}));
  }
  MymBroker broker_l2a(destCF1_name, /*cf_created=*/true,
                        db_path_.c_str(), opts_l2a, 2);

  auto* db2a    = broker_l2a.GetDB();
  auto* l2a_src = broker_l2a.GetCFHandle(destCF1_name);
  const std::string destCF21_name = destCF1_name + "_split0_cf";
  const std::string destCF22_name = destCF1_name + "_split1_cf";
  auto* destCF21 = broker_l2a.GetCFHandle(destCF21_name);
  auto* destCF22 = broker_l2a.GetCFHandle(destCF22_name);
  ASSERT_NE(db2a,     nullptr);
  ASSERT_NE(l2a_src,  nullptr);
  ASSERT_NE(destCF21, nullptr);
  ASSERT_NE(destCF22, nullptr);

  ForceBottommost(db2a, l2a_src);

  // ── Level 2b: destCF2{col2,col3} → destCF23{col2} + destCF24{col3} ───────
  ROCKSDB_NAMESPACE::Options opts_l2b;
  opts_l2b.create_if_missing        = false;
  opts_l2b.disable_auto_compactions = true;
  opts_l2b.num_levels               = 4;
  opts_l2b.compaction_style = ROCKSDB_NAMESPACE::kCompactionStyleLevel;
  opts_l2b.info_log_level   = ROCKSDB_NAMESPACE::InfoLogLevel::FATAL_LEVEL;
  {
    mycelium::SplitByPositions splits3 = {{0}, {1}};
    opts_l2b.transformers.push_back(
        std::make_shared<mycelium::Distributor>(splits3));
    auto parser3 = std::make_shared<mycelium::JsonColsParser>(2, 0);
    auto enc3    = std::make_shared<mycelium::JsonEncoder>();
    mycelium::Codec in3{parser3, nullptr};
    mycelium::Codec out3{nullptr, enc3};
    std::vector<mycelium::FieldSchema> in_s3 = {{"col2", "string", 0},
                                                 {"col3", "string", 1}};
    std::vector<mycelium::FieldSchema> o23   = {{"col2", "string", 0}};
    std::vector<mycelium::FieldSchema> o24   = {{"col3", "string", 1}};
    opts_l2b.schemaDescriptors.push_back(
        std::make_shared<mycelium::SchemaDescriptor>(
            in3, out3, in_s3,
            std::vector<std::vector<mycelium::FieldSchema>>{o23, o24}));
  }
  MymBroker broker_l2b(destCF2_name, /*cf_created=*/true,
                        db_path_.c_str(), opts_l2b, 2);

  auto* db2b    = broker_l2b.GetDB();
  auto* l2b_src = broker_l2b.GetCFHandle(destCF2_name);
  const std::string destCF23_name = destCF2_name + "_split0_cf";
  const std::string destCF24_name = destCF2_name + "_split1_cf";
  auto* destCF23 = broker_l2b.GetCFHandle(destCF23_name);
  auto* destCF24 = broker_l2b.GetCFHandle(destCF24_name);
  ASSERT_NE(db2b,     nullptr);
  ASSERT_NE(l2b_src,  nullptr);
  ASSERT_NE(destCF23, nullptr);
  ASSERT_NE(destCF24, nullptr);

  ForceBottommost(db2b, l2b_src);

  // ── Verify leaf CFs ───────────────────────────────────────────────────────
  // Each leaf CF must have exactly kN records with exactly one column.
  struct LeafCheck {
    ROCKSDB_NAMESPACE::DB*                  db;
    ROCKSDB_NAMESPACE::ColumnFamilyHandle*  cf;
    std::string                             expected_col;
    std::string                             absent_col;
    std::string                             label;
  };
  std::vector<LeafCheck> leaves = {
      {db2a,  destCF21, "col0", "col1", "destCF21"},
      {db2a,  destCF22, "col1", "col0", "destCF22"},
      {db2b,  destCF23, "col2", "col3", "destCF23"},
      {db2b,  destCF24, "col3", "col2", "destCF24"},
  };

  for (const auto& leaf : leaves) {
    std::set<std::string> seen;
    auto* it = leaf.db->NewIterator(ReadOptions(), leaf.cf);
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      const std::string k   = it->key().ToString();
      const std::string val = it->value().ToString();
      EXPECT_TRUE(seen.insert(k).second)
          << leaf.label << ": duplicate key '" << k << "'";
      EXPECT_NE(val.find(leaf.expected_col), std::string::npos)
          << leaf.label << " missing " << leaf.expected_col << " for key " << k;
      EXPECT_EQ(val.find(leaf.absent_col), std::string::npos)
          << leaf.label << " should not contain " << leaf.absent_col
          << " for key " << k;
    }
    EXPECT_TRUE(it->status().ok());
    delete it;

    EXPECT_EQ(static_cast<int>(seen.size()), kN)
        << leaf.label << " should have exactly " << kN << " records";
    for (const auto& k : keys)
      EXPECT_TRUE(seen.count(k)) << leaf.label << " missing key '" << k << "'";
  }
}

// ── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
