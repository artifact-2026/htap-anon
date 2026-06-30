// Copyright (c) 2024-present, Mycelium Authors.
//
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).

#include "mycelium/transform_epoch_tracker.h"

#include <cstring>

namespace mycelium {

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------

bool TransformEpochTracker::IsTransformed(const std::string& cf_name) const {
  auto it = states_.find(cf_name);
  return it != states_.end() && it->second == TransformState::APPLIED;
}

bool TransformEpochTracker::IsDeferred(const std::string& cf_name) const {
  auto it = states_.find(cf_name);
  return it != states_.end() && it->second == TransformState::DEFERRED;
}

TransformState TransformEpochTracker::GetState(
    const std::string& cf_name) const {
  auto it = states_.find(cf_name);
  if (it == states_.end()) return TransformState::UNKNOWN;
  return it->second;
}

std::vector<std::string> TransformEpochTracker::GetDeferredCFs() const {
  std::vector<std::string> result;
  for (const auto& [name, state] : states_) {
    if (state == TransformState::DEFERRED) {
      result.push_back(name);
    }
  }
  return result;
}

std::vector<std::string> TransformEpochTracker::GetAppliedCFs() const {
  std::vector<std::string> result;
  for (const auto& [name, state] : states_) {
    if (state == TransformState::APPLIED) {
      result.push_back(name);
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

void TransformEpochTracker::MarkTransformed(const std::string& cf_name) {
  states_[cf_name] = TransformState::APPLIED;
}

void TransformEpochTracker::MarkDeferred(const std::string& cf_name) {
  // Never downgrade an already-applied transform to DEFERRED.
  auto it = states_.find(cf_name);
  if (it == states_.end() || it->second != TransformState::APPLIED) {
    states_[cf_name] = TransformState::DEFERRED;
  }
}

void TransformEpochTracker::ClearDeferred() {
  for (auto it = states_.begin(); it != states_.end(); ) {
    if (it->second == TransformState::DEFERRED) {
      it = states_.erase(it);
    } else {
      ++it;
    }
  }
}

void TransformEpochTracker::Reset() { states_.clear(); }

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------
//
// Wire format (all little-endian):
//   uint32_t  num_records
//   per record:
//     uint16_t  name_len
//     char[name_len]  name (UTF-8)
//     uint8_t   state  (TransformState)

static void PutFixed16LE(std::string* dst, uint16_t v) {
  char buf[2];
  buf[0] = static_cast<char>(v & 0xFF);
  buf[1] = static_cast<char>((v >> 8) & 0xFF);
  dst->append(buf, 2);
}

static void PutFixed32LE(std::string* dst, uint32_t v) {
  char buf[4];
  for (int i = 0; i < 4; i++) {
    buf[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
  }
  dst->append(buf, 4);
}

// Cursor helpers now take std::string_view* instead of rocksdb::Slice*.
// string_view::remove_prefix() has identical semantics to Slice::remove_prefix().
static bool GetFixed16LE(std::string_view* input, uint16_t* v) {
  if (input->size() < 2) return false;
  const auto* d = reinterpret_cast<const uint8_t*>(input->data());
  *v = static_cast<uint16_t>(d[0]) | (static_cast<uint16_t>(d[1]) << 8);
  input->remove_prefix(2);
  return true;
}

static bool GetFixed32LE(std::string_view* input, uint32_t* v) {
  if (input->size() < 4) return false;
  const auto* d = reinterpret_cast<const uint8_t*>(input->data());
  *v = 0;
  for (int i = 0; i < 4; i++) {
    *v |= static_cast<uint32_t>(d[i]) << (8 * i);
  }
  input->remove_prefix(4);
  return true;
}

void TransformEpochTracker::EncodeTo(std::string* out) const {
  PutFixed32LE(out, static_cast<uint32_t>(states_.size()));
  for (const auto& [name, state] : states_) {
    PutFixed16LE(out, static_cast<uint16_t>(name.size()));
    out->append(name);
    out->push_back(static_cast<char>(state));
  }
}

mycelium::Status TransformEpochTracker::DecodeFrom(std::string_view input) {
  states_.clear();

  uint32_t num_records = 0;
  if (!GetFixed32LE(&input, &num_records)) {
    return mycelium::Status::Corruption(
        "TransformEpochTracker: truncated record count");
  }

  for (uint32_t i = 0; i < num_records; i++) {
    uint16_t name_len = 0;
    if (!GetFixed16LE(&input, &name_len)) {
      return mycelium::Status::Corruption(
          "TransformEpochTracker: truncated name length");
    }
    if (input.size() < name_len + 1u) {
      return mycelium::Status::Corruption(
          "TransformEpochTracker: truncated name/state");
    }
    std::string name(input.data(), name_len);
    input.remove_prefix(name_len);

    uint8_t state_byte = static_cast<uint8_t>(*input.data());
    input.remove_prefix(1);

    if (state_byte != static_cast<uint8_t>(TransformState::APPLIED) &&
        state_byte != static_cast<uint8_t>(TransformState::DEFERRED) &&
        state_byte != static_cast<uint8_t>(TransformState::UNKNOWN)) {
      return mycelium::Status::Corruption(
          "TransformEpochTracker: unknown state byte");
    }
    states_[name] = static_cast<TransformState>(state_byte);
  }
  return mycelium::Status::OK();
}

}  // namespace mycelium
