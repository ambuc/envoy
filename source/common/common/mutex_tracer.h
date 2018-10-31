#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "absl/base/internal/spinlock.h"
#include "absl/base/thread_annotations.h"

namespace Envoy {

// Encapsulates a contention hook which is registered from getOrCreateTracer() which records
// statistics about that contention.
//
// MutexTracer should be accessed via getOrCreateTracer(), which ensures that the global singleton
// MutexTracer object is always being called. This is necessary because of the type signature which
// absl::RegisterMutexTracer() expects.
//
// *NB*: getOrCreateTracer() is not thread-safe, and should be called once at startup, after which
// the internal contention hook is thread-safe. This is possible by utilizing memory_order_relaxed
// atomic writes.
class MutexTracer final {
public:
  static MutexTracer* getOrCreateTracer();

  // Resets the recorded statistics.
  void reset();

  int64_t numContentions() const { return num_contentions_.load(order_); }
  int64_t currentWaitCycles() const { return current_wait_cycles_.load(order_); }
  int64_t lifetimeWaitCycles() const { return lifetime_wait_cycles_.load(order_); }

private:
  friend class MutexTracerTest;

  // Hook called by absl mutex system once registered from getOrCreateTracer().
  static void contentionHook(const char* msg, const void* obj, int64_t wait_cycles);

  // Utility function for contentionHook.
  inline void recordContention(const char*, const void*, int64_t wait_cycles);

  static MutexTracer* singleton_;

  // Number of mutex contention occurrences since last reset.
  std::atomic<int64_t> num_contentions_{0};
  // Length of the current contention wait cycle.
  std::atomic<int64_t> current_wait_cycles_{0};
  // Total sum of all wait cycles.
  std::atomic<int64_t> lifetime_wait_cycles_{0};
  // TODO(ambuc): Build running averages here?

  // We utilize std::memory_order_relaxed for all operations for the least possible contention.
  std::memory_order order_{std::memory_order_relaxed};
};

} // namespace Envoy
