#pragma once

// mycelium/status.h
//
// Thin, engine-agnostic error type for libmycelium.
//
// Design rationale (see Portability Plan §6.2):
//   - Replaces rocksdb::Status so the portable core has no RocksDB headers.
//   - Kept deliberately minimal (ok/error + message string).  We don't
//     replicate RocksDB's SubCode / Severity hierarchy because libmycelium
//     never needs them.
//   - Arrow is already a hard dependency, but arrow::Status has a richer ABI
//     than we need and couples error-handling to the Arrow version.  A thin
//     wrapper is easier to evolve independently.
//
// Usage:
//   mycelium::Status s = mycelium::Status::OK();
//   if (!s.ok()) { ... s.message() ... }

#include <string>
#include <utility>

namespace mycelium {

class Status {
 public:
  // ── Factories ────────────────────────────────────────────────────────────
  static Status OK() noexcept { return Status{}; }

  static Status Error(std::string msg) {
    Status s;
    s.msg_ = std::move(msg);
    s.ok_  = false;
    return s;
  }

  static Status IOError(std::string msg) {
    return Error("IOError: " + std::move(msg));
  }

  static Status Corruption(std::string msg) {
    return Error("Corruption: " + std::move(msg));
  }

  static Status InvalidArgument(std::string msg) {
    return Error("InvalidArgument: " + std::move(msg));
  }

  static Status NotSupported(std::string msg) {
    return Error("NotSupported: " + std::move(msg));
  }

  // ── Observers ────────────────────────────────────────────────────────────
  bool ok() const noexcept { return ok_; }

  // Returns empty string when ok().
  const std::string& message() const noexcept { return msg_; }

  // Implicit bool conversion so callers can write: if (!s) { ... }
  explicit operator bool() const noexcept { return ok_; }

 private:
  Status() = default;

  bool        ok_  = true;
  std::string msg_;
};

// Convenience: propagate a non-OK status early.
// Usage:  MYCELIUM_RETURN_IF_ERROR(SomeCall());
#define MYCELIUM_RETURN_IF_ERROR(expr)          \
  do {                                          \
    ::mycelium::Status _s = (expr);             \
    if (!_s.ok()) return _s;                    \
  } while (false)

}  // namespace mycelium
