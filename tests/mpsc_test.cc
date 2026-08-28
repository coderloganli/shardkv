// Test cases 1-11 from task.md.
//
// The queue and the wakeup rule. This is the code in the project most able to
// hide a subtle bug, and the tooling is uneven: ThreadSanitizer checks the data
// races, but nothing checks for a lost wakeup. So the wakeup rule is hammered
// rather than inspected.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "base/mpsc_queue.h"

using namespace shardkv;

namespace {

struct Payload {
  int producer = 0;
  int seq = 0;
};

using Queue = MpscQueue<Payload>;

Queue::Node* makeNode(int producer, int seq) {
  auto* node = new Queue::Node();
  node->value = Payload{producer, seq};
  return node;
}

}  // namespace

// 1
TEST(MpscQueue, EmptyQueuePopsNothing) {
  Queue q;
  EXPECT_EQ(q.pop(), nullptr);
}

// 2
TEST(MpscQueue, SingleProducerPreservesOrder) {
  Queue q;
  for (int i = 1; i <= 1000; ++i) q.push(makeNode(0, i));

  for (int expected = 1; expected <= 1000; ++expected) {
    Queue::Node* node = q.pop();
    ASSERT_NE(node, nullptr) << "missing element " << expected;
    EXPECT_EQ(node->value.seq, expected);
    delete node;
  }
  EXPECT_EQ(q.pop(), nullptr);
}

// 3 -- the basis of the wakeup rule. It has to be push's answer: asking
// afterwards is a different question, and by then the consumer may have drained.
TEST(MpscQueue, PushReportsWhetherQueueWasEmpty) {
  Queue q;
  EXPECT_TRUE(q.push(makeNode(0, 1))) << "the first push finds the queue empty";
  EXPECT_FALSE(q.push(makeNode(0, 2))) << "the second does not";

  delete q.pop();
  delete q.pop();
  EXPECT_TRUE(q.push(makeNode(0, 3))) << "empty again after draining";
  delete q.pop();
}

// 4 -- MPSC promises order within a producer, not between producers.
TEST(MpscQueue, ManyProducersLoseNothing) {
  constexpr int kProducers = 8;
  constexpr int kPerProducer = 10000;

  Queue q;
  std::atomic<int> taken{0};
  std::vector<int> last_seen(kProducers, 0);

  std::thread consumer([&] {
    while (taken.load(std::memory_order_relaxed) < kProducers * kPerProducer) {
      Queue::Node* node = q.pop();
      if (node == nullptr) {
        std::this_thread::yield();
        continue;
      }
      const Payload p = node->value;
      delete node;
      EXPECT_GT(p.seq, last_seen[p.producer])
          << "producer " << p.producer << " arrived out of order";
      last_seen[p.producer] = p.seq;
      taken.fetch_add(1, std::memory_order_relaxed);
    }
  });

  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&q, p] {
      for (int i = 1; i <= kPerProducer; ++i) q.push(makeNode(p, i));
    });
  }
  for (auto& t : producers) t.join();
  consumer.join();

  EXPECT_EQ(taken.load(), kProducers * kPerProducer);
  for (int p = 0; p < kProducers; ++p) EXPECT_EQ(last_seen[p], kPerProducer);
}

// 5 -- not a duplicate of 4. That one proves nothing is lost; this one proves
// there is no data race while it happens. Only meaningful in the TSan build,
// where it either reports or it does not.
TEST(MpscQueue, ManyProducersUnderTsanIsClean) {
  constexpr int kProducers = 4;
  constexpr int kPerProducer = 2000;

  Queue q;
  std::atomic<int> taken{0};

  std::thread consumer([&] {
    while (taken.load(std::memory_order_relaxed) < kProducers * kPerProducer) {
      Queue::Node* node = q.pop();
      if (node == nullptr) {
        std::this_thread::yield();
        continue;
      }
      delete node;
      taken.fetch_add(1, std::memory_order_relaxed);
    }
  });

  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&q, p] {
      for (int i = 1; i <= kPerProducer; ++i) q.push(makeNode(p, i));
    });
  }
  for (auto& t : producers) t.join();
  consumer.join();
  EXPECT_EQ(taken.load(), kProducers * kPerProducer);
}

// 6
TEST(MpscQueue, PopDrainsToEmptyThenReportsEmpty) {
  Queue q;
  for (int i = 0; i < 50; ++i) q.push(makeNode(0, i));
  for (int i = 0; i < 50; ++i) {
    Queue::Node* node = q.pop();
    ASSERT_NE(node, nullptr);
    delete node;
  }
  EXPECT_EQ(q.pop(), nullptr);
}

// 7 -- a producer that has claimed its place but not yet linked itself in makes
// the queue look briefly empty. A consumer that reads that as final will stall
// with work still queued, so what is asserted is that it recovers.
TEST(MpscQueue, ConsumerSeesEmptyDuringProducerWindowButRecovers) {
  constexpr int kCount = 10000;
  Queue q;
  std::atomic<int> taken{0};
  std::atomic<int> transient_empties{0};

  std::thread consumer([&] {
    while (taken.load(std::memory_order_relaxed) < kCount) {
      Queue::Node* node = q.pop();
      if (node == nullptr) {
        transient_empties.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::yield();
        continue;
      }
      delete node;
      taken.fetch_add(1, std::memory_order_relaxed);
    }
  });

  std::thread producer([&] {
    for (int i = 0; i < kCount; ++i) q.push(makeNode(0, i));
  });

  producer.join();
  consumer.join();
  EXPECT_EQ(taken.load(), kCount) << "the consumer stalled on a transient empty";
}

// 8 -- the ownership contract: a producer hands a node over and never touches
// it again; the consumer frees what it pops. Under ASan a breach is a leak.
TEST(MpscQueue, ConsumerFreesPoppedNodes) {
  Queue q;
  for (int i = 0; i < 1000; ++i) q.push(makeNode(0, i));
  int freed = 0;
  while (Queue::Node* node = q.pop()) {
    delete node;
    ++freed;
  }
  EXPECT_EQ(freed, 1000);
}

// 9 -- and whatever is left when the queue dies is the queue's to free.
TEST(MpscQueue, DrainOnShutdownFreesRemainingNodes) {
  {
    Queue q;
    for (int i = 0; i < 1000; ++i) q.push(makeNode(0, i));
    // Deliberately popped by nobody.
  }
  SUCCEED() << "leaks are reported by ASan, not by this assertion";
}

// ------------------------------------------------------- the wakeup rule

namespace {

// Stands in for the eventfd so a test can count wakeups instead of watching a
// descriptor.
struct CountingWaker {
  std::atomic<int> wakeups{0};
  void wake() { wakeups.fetch_add(1, std::memory_order_relaxed); }
};

// The rule under test, expressed once: signal only on the empty-to-non-empty
// transition, because a consumer that has not drained yet is already awake or
// already has a wakeup pending.
void pushAndMaybeWake(Queue& q, Queue::Node* node, CountingWaker& waker) {
  if (q.push(node)) waker.wake();
}

}  // namespace

// 10
TEST(Wakeup, WakeupIsElidedWhenQueueWasNotEmpty) {
  Queue q;
  CountingWaker waker;

  pushAndMaybeWake(q, makeNode(0, 1), waker);
  pushAndMaybeWake(q, makeNode(0, 2), waker);

  EXPECT_EQ(waker.wakeups.load(), 1) << "the second push must not signal again";

  delete q.pop();
  delete q.pop();
}

// 11 -- a lost wakeup corrupts nothing. It hangs, under one interleaving, on
// someone else's machine. So this hammers the transition rather than reasoning
// about it.
TEST(Wakeup, WakeupIsNotLostOnEmptyToNonEmptyTransition) {
  constexpr int kRounds = 20000;
  Queue q;
  CountingWaker waker;

  for (int i = 0; i < kRounds; ++i) {
    // Drain to empty, then push exactly one. Every round must signal.
    while (Queue::Node* node = q.pop()) delete node;
    pushAndMaybeWake(q, makeNode(0, i), waker);
  }
  while (Queue::Node* node = q.pop()) delete node;

  EXPECT_EQ(waker.wakeups.load(), kRounds)
      << "a push onto an empty queue did not signal";
}

// Added in stage 7, after a one-in-twelve timeout in the multi-loop tests.
//
// The bug was not a race and ThreadSanitizer had nothing to say about it: a
// consumer treated "a producer is mid-push" as "the queue is empty", went back
// to sleep, and was never woken -- because the producer in the window had
// already decided whether to signal, and the producer behind it saw a
// non-empty queue and decided not to.
//
// This drives the same shape: producers signal only on the empty-to-non-empty
// transition, and the consumer sleeps whenever it believes the queue is empty.
// A lost wakeup shows up as items that never arrive rather than as a hang, so
// the failure is reported instead of hanging the suite.
TEST(Wakeup, ConsumerNeverSleepsWhileWorkIsQueued) {
  constexpr int kProducers = 4;
  constexpr int kPerProducer = 5000;
  constexpr int kTotal = kProducers * kPerProducer;

  Queue q;
  std::atomic<int> signals{0};
  std::atomic<int> taken{0};
  std::atomic<bool> producers_done{false};

  std::thread consumer([&] {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (taken.load(std::memory_order_relaxed) < kTotal &&
           std::chrono::steady_clock::now() < deadline) {
      bool busy = false;
      Queue::Node* node = q.pop(&busy);
      if (node != nullptr) {
        delete node;
        taken.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (busy) {
        // Work is queued; a consumer that slept here would be relying on a
        // wakeup nobody is obliged to send.
        std::this_thread::yield();
        continue;
      }
      if (producers_done.load(std::memory_order_acquire) &&
          taken.load(std::memory_order_relaxed) >= kTotal) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(50));  // "asleep"
    }
  });

  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      for (int i = 1; i <= kPerProducer; ++i) {
        if (q.push(makeNode(p, i))) signals.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto& t : producers) t.join();
  producers_done.store(true, std::memory_order_release);
  consumer.join();

  EXPECT_EQ(taken.load(), kTotal)
      << "items were left queued with the consumer asleep -- a lost wakeup";
  EXPECT_LT(signals.load(), kTotal)
      << "every push signalled, so the elision rule was not exercised at all";
}
