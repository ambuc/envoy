#include <chrono>
#include <thread>

#include "common/common/mutex_tracer.h"

#include "absl/synchronization/mutex.h"
#include "gtest/gtest.h"

namespace Envoy {

class MutexTracerTest : public testing::Test {
protected:
  void SetUp() {
    MutexTracer::getOrCreateTracer()->Reset();
    absl::RegisterMutexTracer(&MutexTracer::contentionHook);
  }

  absl::Mutex mu_;
};

// Call the contention hook manually.
TEST_F(MutexTracerTest, AddN) {
  EXPECT_EQ(MutexTracer::getOrCreateTracer()->numContentions(), 0);
  EXPECT_EQ(MutexTracer::getOrCreateTracer()->currentWaitCycles(), 0);
  EXPECT_EQ(MutexTracer::getOrCreateTracer()->lifetimeWaitCycles(), 0);

  MutexTracer::getOrCreateTracer()->contentionHook(nullptr, nullptr, 2);

  EXPECT_EQ(MutexTracer::getOrCreateTracer()->numContentions(), 1);
  EXPECT_EQ(MutexTracer::getOrCreateTracer()->currentWaitCycles(), 2);
  EXPECT_EQ(MutexTracer::getOrCreateTracer()->lifetimeWaitCycles(), 2);

  MutexTracer::getOrCreateTracer()->contentionHook(nullptr, nullptr, 3);

  EXPECT_EQ(MutexTracer::getOrCreateTracer()->numContentions(), 2);
  EXPECT_EQ(MutexTracer::getOrCreateTracer()->currentWaitCycles(), 3);
  EXPECT_EQ(MutexTracer::getOrCreateTracer()->lifetimeWaitCycles(), 5);

  MutexTracer::getOrCreateTracer()->contentionHook(nullptr, nullptr, 0);

  EXPECT_EQ(MutexTracer::getOrCreateTracer()->numContentions(), 3);
  EXPECT_EQ(MutexTracer::getOrCreateTracer()->currentWaitCycles(), 0);
  EXPECT_EQ(MutexTracer::getOrCreateTracer()->lifetimeWaitCycles(), 5);
}

void holdMutexForNMilliseconds(absl::Mutex* mu, int64_t duration) {
  mu->Lock();
  std::this_thread::sleep_for(std::chrono::milliseconds(duration));
  mu->Unlock();
}

// Call the contention hook in a real contention scenario.
TEST_F(MutexTracerTest, OneThreadNoContention) {
  // Regular operation doesn't cause contention.
  mu_.Lock();
  mu_.Unlock();

  EXPECT_EQ(MutexTracer::getOrCreateTracer()->numContentions(), 0);
  EXPECT_EQ(MutexTracer::getOrCreateTracer()->currentWaitCycles(), 0);
  EXPECT_EQ(MutexTracer::getOrCreateTracer()->lifetimeWaitCycles(), 0);
}

TEST_F(MutexTracerTest, TryLockNoContention) {
  // TryLocks don't cause contention.
  mu_.Lock();
  EXPECT_FALSE(mu_.TryLock());
  mu_.Unlock();

  EXPECT_EQ(MutexTracer::getOrCreateTracer()->numContentions(), 0);
  EXPECT_EQ(MutexTracer::getOrCreateTracer()->currentWaitCycles(), 0);
  EXPECT_EQ(MutexTracer::getOrCreateTracer()->lifetimeWaitCycles(), 0);
}

TEST_F(MutexTracerTest, TwoThreadsWithContention) {
  // Real mutex contention successfully triggers this callback.
  std::thread t1(holdMutexForNMilliseconds, &mu_, 10);
  std::thread t2(holdMutexForNMilliseconds, &mu_, 10);
  t1.join();
  t2.join();

  EXPECT_EQ(MutexTracer::getOrCreateTracer()->numContentions(), 1);
  EXPECT_GT(MutexTracer::getOrCreateTracer()->currentWaitCycles(),
            0); // These shouldn't be hardcoded.
  EXPECT_GT(MutexTracer::getOrCreateTracer()->lifetimeWaitCycles(), 0);

  // If we store this for later,
  int64_t prev_lifetime_wait_cycles = MutexTracer::getOrCreateTracer()->lifetimeWaitCycles();

  // Then on our next call...
  std::thread t3(holdMutexForNMilliseconds, &mu_, 10);
  std::thread t4(holdMutexForNMilliseconds, &mu_, 10);
  t3.join();
  t4.join();

  EXPECT_EQ(MutexTracer::getOrCreateTracer()->numContentions(), 2);
  EXPECT_GT(MutexTracer::getOrCreateTracer()->currentWaitCycles(), 0);
  EXPECT_GT(MutexTracer::getOrCreateTracer()->lifetimeWaitCycles(), 0);
  // ...we can confirm that this lifetime value went up.
  EXPECT_GT(MutexTracer::getOrCreateTracer()->lifetimeWaitCycles(), prev_lifetime_wait_cycles);
}

} // namespace Envoy
