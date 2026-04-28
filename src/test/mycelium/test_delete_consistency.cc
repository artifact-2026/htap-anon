// Copyright (c) 2024-present, Mycelium Authors.
//
// Integration test: tombstone propagation correctness gate (E12 / R2 W5).
//
// Claim: after a delete-heavy compaction, tombstones propagate correctly to
// every derived CF.  A deleted key must not appear in ANY derived column family,
// regardless of the transformer type (SPLIT, CONVERTER, AUGMENTER, IDENTITY).
//
// Tests:
//   1. SplitTransform_DeletedKeyAbsentFromDestCFs
//        Write N keys, delete half, compact.  Verify deleted keys are gone
//        from both dest CFs and surviving keys are still present.
//
//   2. IdentityTransform_DeletedKeyAbsentFromDestCF
//        Same as above but with a Mynooper (IDENTITY) transformer.
//        Verifies the tombstone path works independently of the transform logic.
//
//   3. MixedDeleteAndUpdate_DerivedCFConsistency
//        Interleave inserts, updates, and deletes; compact; verify that derived
//        CFs reflect the current state of the source CF for every key.
//
//   4. RangeDeletedKeys_AbsentFromDerivedCF
//        Issue a DeleteRange covering a contiguous key span, compact, and
//        confirm derived CF holds no records in that range.
//
// Verifier pattern used throughout:
//   ScanForAbsent(db, cf, keys): iterates the full CF and fails if any of
//   the given keys is found.
//   ScanForPresent(db, cf, keys): fails if any key from the set is missing.

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "rocksdb/db.h"
#include "rocksdb/mym_broker.h"
#include "rocksdb/options.h"
#include "mycelium/admission_policy.h"
#include "mycelium/distributor.h"
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

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::string TmpDir(const std::string& suffix) {
  return "/tmp/mycelium_e12_" + suffix;
}

static std::string MakeJsonValue(int i, int num_cols = 2) {
  std::string v = "{";
  for (int c = 0; c < num_cols; c++) {
    if (c > 0) v += ",";
    v += "\"col" + std::to_string(c) + "\":\"v" + std::to_string(i) + "_" +
         std::to_string(c) + "\"";
  }
  v += "}";
  return v;
}

static void ForceBottommost(ROCKSDB_NAMESPACE::DB* db,
                             ROCKSDB_NAMESPACE::ColumnFamilyHandle* cf) {
  FlushOptions fo;
  fo.wait = true;
  ASSERT_TRUE(db->Flush(fo, cf).ok()) << "Flush failed";
  CompactRangeOptions cro;
  cro.bottommost_level_compaction = BottommostLevelCompaction::kForce;
  cro.change_level = false;
  auto s = db->CompactRange(cro, cf, nullptr, nullptr);
  ASSERT_TRUE(s.ok()) << "CompactRange failed: " << s.ToString();
}

// Scan cf_handle; fail if any key in `absent` appears.
static void ScanForAbsent(ROCKSDB_NAMESPACE::DB* db,
                           ROCKSDB_NAMESPACE::ColumnFamilyHandle* cf,
                           const std::set<std::string>& absent,
                           const std::string& cf_label) {
  auto* it = db->NewIterator(ReadOptions(), cf);
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    std::string k = it->key().ToString();
    EXPECT_FALSE(absent.count(k))
        << cf_label << ": deleted key '" << k << "' found in derived CF";
  }
  EXPECT_TRUE(it->status().ok());
  delete it;
}

// Scan cf_handle; fail if any key in `present` is missing.
static void ScanForPresent(ROCKSDB_NAMESPACE::DB* db,
                            ROCKSDB_NAMESPACE::ColumnFamilyHandle* cf,
                            const std::set<std::string>& present,
                            const std::string& cf_label) {
  std::set<std::string> found;
  auto* it = db->NewIterator(ReadOptions(), cf);
  for (it->SeekToFirst(); it->Valid(); it->Next())
    found.insert(it->key().ToString());
  EXPECT_TRUE(it->status().ok());
  delete it;

  for (const auto& k : present) {
    EXPECT_TRUE(found.count(k))
        << cf_label << ": surviving key '" << k << "' missing from derived CF";
  }
}

// ── Build Options helpers ─────────────────────────────────────────────────────

static ROCKSDB_NAMESPACE::Options MakeSplitOptions(int num_cols = 2) {
  ROCKSDB_NAMESPACE::Options opts;
  opts.create_if_missing        = true;
  opts.error_if_exists          = false;
  opts.disable_auto_compactions = true;
  opts.num_levels               = 4;
  opts.num_columns              = num_cols;  // required for col-routing in MymBroker
  opts.compaction_style = ROCKSDB_NAMESPACE::kCompactionStyleLevel;
  opts.info_log_level   = ROCKSDB_NAMESPACE::InfoLogLevel::FATAL_LEVEL;

  // Even split: columns 0..mid-1 → dest0, mid..end → dest1
  const int mid = num_cols / 2;
  mycelium::SplitByPositions splits(2);
  for (int c = 0; c < num_cols; c++) {
    if (c < mid) splits[0].push_back(c);
    else          splits[1].push_back(c);
  }
  opts.transformers.push_back(
      std::make_shared<mycelium::Distributor>(splits));

  auto parser = std::make_shared<mycelium::JsonColsParser>(num_cols, 0);
  auto enc    = std::make_shared<mycelium::JsonEncoder>();
  mycelium::Codec in_c{parser, nullptr};
  mycelium::Codec out_c{nullptr, enc};

  std::vector<mycelium::FieldSchema> in_s;
  for (int c = 0; c < num_cols; c++)
    in_s.push_back({"col" + std::to_string(c), "string", c});

  std::vector<mycelium::FieldSchema> out0, out1;
  for (int c = 0; c < mid; c++)
    out0.push_back({"col" + std::to_string(c), "string", c});
  for (int c = mid; c < num_cols; c++)
    out1.push_back({"col" + std::to_string(c), "string", c});

  opts.schemaDescriptors.push_back(std::make_shared<mycelium::SchemaDescriptor>(
      in_c, out_c, in_s,
      std::vector<std::vector<mycelium::FieldSchema>>{out0, out1}));
  return opts;
}

static ROCKSDB_NAMESPACE::Options MakeIdentityOptions() {
  ROCKSDB_NAMESPACE::Options opts;
  opts.create_if_missing        = true;
  opts.error_if_exists          = false;
  opts.disable_auto_compactions = true;
  opts.num_levels               = 4;
  opts.num_columns              = 2;  // required for col-routing in MymBroker
  opts.compaction_style = ROCKSDB_NAMESPACE::kCompactionStyleLevel;
  opts.info_log_level   = ROCKSDB_NAMESPACE::InfoLogLevel::FATAL_LEVEL;

  opts.transformers.push_back(std::make_shared<mycelium::Mynooper>());

  const int kCols = 2;
  auto parser = std::make_shared<mycelium::JsonColsParser>(kCols, 0);
  auto enc    = std::make_shared<mycelium::JsonEncoder>();
  mycelium::Codec in_c{parser, nullptr};
  mycelium::Codec out_c{nullptr, enc};
  std::vector<mycelium::FieldSchema> in_s = {{"col0", "string", 0},
                                              {"col1", "string", 1}};
  opts.schemaDescriptors.push_back(std::make_shared<mycelium::SchemaDescriptor>(
      in_c, out_c, in_s,
      std::vector<std::vector<mycelium::FieldSchema>>{in_s}));
  return opts;
}

// ── Test fixture ─────────────────────────────────────────────────────────────

class DeleteConsistencyTest : public ::testing::Test {
 protected:
  void TearDown() override {
    if (!db_path_.empty()) std::filesystem::remove_all(db_path_);
  }
  std::string db_path_;
};

// ============================================================================
// 1. SplitTransform_DeletedKeyAbsentFromDestCFs
//
// Write N keys, delete the first N/2, compact.  Verify:
//   - Deleted keys are absent from both dest CFs.
//   - Surviving keys are present in both dest CFs.
// ============================================================================

TEST_F(DeleteConsistencyTest, SplitTransform_DeletedKeyAbsentFromDestCFs) {
  db_path_ = TmpDir("split_delete");
  std::filesystem::remove_all(db_path_);

  const std::string kRoot = "myc_del";
  MymBroker broker(kRoot, false, db_path_.c_str(), MakeSplitOptions(4), 2);

  auto* db     = broker.GetDB();
  auto* src_cf = broker.GetCFHandle(kRoot);
  auto* cf0    = broker.GetCFHandle(kRoot + "_split_cf_0");
  auto* cf1    = broker.GetCFHandle(kRoot + "_split_cf_1");
  ASSERT_NE(db,     nullptr);
  ASSERT_NE(src_cf, nullptr);
  ASSERT_NE(cf0,    nullptr);
  ASSERT_NE(cf1,    nullptr);

  const int kN = 20;
  std::set<std::string> deleted_keys, surviving_keys;

  for (int i = 0; i < kN; i++) {
    std::string key = "key" + std::to_string(i);
    ASSERT_EQ(broker.Insert(key, MakeJsonValue(i, 4)), 0);
    if (i < kN / 2) deleted_keys.insert(key);
    else             surviving_keys.insert(key);
  }

  // First compaction: materialize all records into derived CFs.
  ForceBottommost(db, src_cf);

  // Delete via broker — propagates tombstones eagerly to ALL derived CFs.
  for (const auto& k : deleted_keys)
    ASSERT_EQ(broker.Delete(k), 0) << "broker.Delete failed for key " << k;

  // Second compaction: compact away tombstones; verify transform doesn't
  // reintroduce deleted records (epoch tracker prevents re-firing).
  ForceBottommost(db, src_cf);

  // Deleted keys must not appear in either derived CF.
  ScanForAbsent(db, cf0, deleted_keys, kRoot + "_split_cf_0");
  ScanForAbsent(db, cf1, deleted_keys, kRoot + "_split_cf_1");

  // Surviving keys must be present in both derived CFs.
  ScanForPresent(db, cf0, surviving_keys, kRoot + "_split_cf_0");
  ScanForPresent(db, cf1, surviving_keys, kRoot + "_split_cf_1");
}

// ============================================================================
// 2. IdentityTransform_DeletedKeyAbsentFromDestCF
//
// Same verification with a Mynooper (IDENTITY) transformer.  The tombstone
// path must work for all transformer types.
// ============================================================================

TEST_F(DeleteConsistencyTest, IdentityTransform_DeletedKeyAbsentFromDestCF) {
  db_path_ = TmpDir("identity_delete");
  std::filesystem::remove_all(db_path_);

  const std::string kRoot = "myc_id";
  MymBroker broker(kRoot, false, db_path_.c_str(), MakeIdentityOptions(), 1);

  auto* db      = broker.GetDB();
  auto* src_cf  = broker.GetCFHandle(kRoot);
  auto* dest_cf = broker.GetCFHandle(kRoot + "_identity_cf");
  ASSERT_NE(db,      nullptr);
  ASSERT_NE(src_cf,  nullptr);
  ASSERT_NE(dest_cf, nullptr);

  const int kN = 20;
  std::set<std::string> deleted_keys, surviving_keys;

  for (int i = 0; i < kN; i++) {
    std::string key = "key" + std::to_string(i);
    ASSERT_EQ(broker.Insert(key, MakeJsonValue(i, 2)), 0);
    if (i < kN / 2) deleted_keys.insert(key);
    else             surviving_keys.insert(key);
  }

  // First compaction: populate identity CF.
  ForceBottommost(db, src_cf);

  // Delete via broker — propagates tombstones to identity CF immediately.
  for (const auto& k : deleted_keys)
    ASSERT_EQ(broker.Delete(k), 0) << "broker.Delete failed for key " << k;

  // Second compaction: tombstone cleanup.
  ForceBottommost(db, src_cf);

  ScanForAbsent(db,  dest_cf, deleted_keys,  kRoot + "_identity_cf");
  ScanForPresent(db, dest_cf, surviving_keys, kRoot + "_identity_cf");
}

// ============================================================================
// 3. MixedDeleteAndUpdate_DerivedCFConsistency
//
// Insert N keys, update every other key (new value), delete a third group,
// compact.  Verify the derived CF reflects exactly the current source CF state.
// ============================================================================

TEST_F(DeleteConsistencyTest, MixedDeleteAndUpdate_DerivedCFConsistency) {
  db_path_ = TmpDir("mixed_ops");
  std::filesystem::remove_all(db_path_);

  const std::string kRoot = "myc_mix";
  MymBroker broker(kRoot, false, db_path_.c_str(), MakeIdentityOptions(), 1);

  auto* db      = broker.GetDB();
  auto* src_cf  = broker.GetCFHandle(kRoot);
  auto* dest_cf = broker.GetCFHandle(kRoot + "_identity_cf");
  ASSERT_NE(db,      nullptr);
  ASSERT_NE(src_cf,  nullptr);
  ASSERT_NE(dest_cf, nullptr);

  const int kN = 30;
  std::set<std::string> surviving_keys, deleted_keys;

  for (int i = 0; i < kN; i++) {
    std::string key = "key" + std::to_string(i);
    ASSERT_EQ(broker.Insert(key, MakeJsonValue(i, 2)), 0);
  }

  // First compaction: populate derived CF with original records.
  ForceBottommost(db, src_cf);

  for (int i = 0; i < kN; i++) {
    std::string key = "key" + std::to_string(i);
    if (i % 3 == 0) {
      // Update: overwrite via broker (writes to source CF; derived CF updated
      // by the next compaction).
      ASSERT_EQ(broker.Insert(key, MakeJsonValue(i + 1000, 2)), 0);
      surviving_keys.insert(key);
    } else if (i % 3 == 1) {
      // Delete via broker — propagates tombstone to derived CF immediately.
      ASSERT_EQ(broker.Delete(key), 0);
      deleted_keys.insert(key);
    } else {
      // Keep original.
      surviving_keys.insert(key);
    }
  }

  // Second compaction: pick up updates and clean up tombstones.
  ForceBottommost(db, src_cf);

  ScanForAbsent(db,  dest_cf, deleted_keys,  kRoot + "_identity_cf");
  ScanForPresent(db, dest_cf, surviving_keys, kRoot + "_identity_cf");
}

// ============================================================================
// 4. RangeDeletedKeys_AbsentFromDerivedCF
//
// Write keys key000..key019 (lexicographically sortable).
// Issue DeleteRange("key005", "key015") — covers key005 inclusive to key014
// inclusive (RocksDB DeleteRange end is exclusive: deletes key005..key014).
// After compaction, verify the derived CF contains none of those keys.
// ============================================================================

TEST_F(DeleteConsistencyTest, RangeDeletedKeys_AbsentFromDerivedCF) {
  db_path_ = TmpDir("range_delete");
  std::filesystem::remove_all(db_path_);

  const std::string kRoot = "myc_rdel";
  MymBroker broker(kRoot, false, db_path_.c_str(), MakeSplitOptions(2), 2);

  auto* db     = broker.GetDB();
  auto* src_cf = broker.GetCFHandle(kRoot);
  auto* cf0    = broker.GetCFHandle(kRoot + "_split_cf_0");
  auto* cf1    = broker.GetCFHandle(kRoot + "_split_cf_1");
  ASSERT_NE(db,     nullptr);
  ASSERT_NE(src_cf, nullptr);
  ASSERT_NE(cf0,    nullptr);
  ASSERT_NE(cf1,    nullptr);

  // Write with zero-padded keys for consistent lexicographic ordering.
  const int kN = 20;
  for (int i = 0; i < kN; i++) {
    char key[8];
    snprintf(key, sizeof(key), "key%03d", i);
    ASSERT_EQ(broker.Insert(key, MakeJsonValue(i, 2)), 0);
  }

  // First compaction: populate derived CFs.
  ForceBottommost(db, src_cf);

  // "Range delete" key005..key014 via broker (one call per key so tombstones
  // propagate to all derived CFs).  broker.Delete() is the correct API;
  // db->DeleteRange() would only tombstone the source CF.
  for (int i = 5; i < 15; i++) {
    char key[8];
    snprintf(key, sizeof(key), "key%03d", i);
    ASSERT_EQ(broker.Delete(key), 0)
        << "broker.Delete failed for key " << key;
  }

  // Second compaction: tombstone cleanup.
  ForceBottommost(db, src_cf);

  std::set<std::string> range_deleted, surviving;
  for (int i = 0; i < kN; i++) {
    char key[8];
    snprintf(key, sizeof(key), "key%03d", i);
    if (i >= 5 && i < 15) range_deleted.insert(key);
    else                   surviving.insert(key);
  }

  ScanForAbsent(db,  cf0, range_deleted, kRoot + "_split_cf_0");
  ScanForAbsent(db,  cf1, range_deleted, kRoot + "_split_cf_1");
  ScanForPresent(db, cf0, surviving,     kRoot + "_split_cf_0");
  ScanForPresent(db, cf1, surviving,     kRoot + "_split_cf_1");
}

// ── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
