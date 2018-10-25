#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "absl/base/internal/spinlock.h"
#include "absl/base/thread_annotations.h"

namespace Envoy {

// Encapsulates a contention hook which can be registered with absl::RegisterMutexTracer() which
// records statistics about that contention.
//
// MutexTracer should be accessed via getOrCreateTracer(), which ensures that the global singleton
// MutexTracer object is always being called. This is necessary because of the type signature which
// absl::RegisterMutexTracer() expects.
//
// This class is thread-safe, and can be called from multiple mutexes in contention across multiple
// threads. This is made possible by utilizing memory_order_relaxed atomic writes.
class MutexTracer final {
public:
  static MutexTracer* getOrCreateTracer();

  // Returns the callback which can be registered via
  // absl::RegisterMutexTracer(&Envoy::MutexTracer::contentionHook).
  static void contentionHook(const char* msg, const void* obj, int64_t wait_cycles);

  // Resets the recorded statistics.
  void Reset();

  int64_t numContentions() const { return num_contentions_.load(order_); }
  int64_t currentWaitCycles() const { return current_wait_cycles_.load(order_); }
  int64_t lifetimeWaitCycles() const { return lifetime_wait_cycles_.load(order_); }

private:
  static MutexTracer* singleton_;

  // Utility function for contentionHook.
  void RecordContention(const char*, const void*, int64_t wait_cycles);

  // Number of mutex contention occurrences since last reset.
  std::atomic<int64_t> num_contentions_;
  // Length of the current contention wait cycle.
  std::atomic<int64_t> current_wait_cycles_;
  // Total sum of all wait cycles.
  std::atomic<int64_t> lifetime_wait_cycles_;
  // TODO(ambuc): Build running averages here?

  // We utilize std::memory_order_relaxed for all operations for the least possible contention.
  std::memory_order order_{std::memory_order_relaxed};
};

} // namespace Envoy
