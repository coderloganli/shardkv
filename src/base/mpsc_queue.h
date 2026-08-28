#pragma once

#include <atomic>
#include <utility>

namespace shardkv {

// Intrusive multi-producer, single-consumer queue, of the shape Dmitry Vyukov
// described: producers swap themselves onto the tail, the consumer walks a
// list terminated by a permanently-present stub.
//
// See docs/adr/0007-a-lock-free-mpsc-queue-carries-cross-shard-messages.md for
// why this is lock-free rather than a mutex and a deque.
//
// OWNERSHIP. A producer allocates a node and hands it to push(), after which it
// must not touch it again -- the consumer may already have freed it. The
// consumer owns whatever pop() returns and must delete it. The destructor
// drains and deletes whatever is left.
//
// MEMORY ORDER. Every operation below says what it orders and why. relaxed is
// used only where there is genuinely nothing to order, never because it is
// cheaper.
template <typename T>
class MpscQueue {
 public:
  struct Node {
    std::atomic<Node*> next{nullptr};
    T value{};
  };

  MpscQueue() : head_(&stub_), tail_(&stub_) {}

  ~MpscQueue() {
    // Whatever nobody popped is this queue's to free.
    while (Node* node = pop()) delete node;
  }

  MpscQueue(const MpscQueue&) = delete;
  MpscQueue& operator=(const MpscQueue&) = delete;

  // Takes ownership of `node`. Returns whether the queue was empty beforehand,
  // which is what the wakeup rule keys on: a producer signals the consumer only
  // on the empty-to-non-empty transition, because a consumer that has not
  // drained yet is either awake or already has a wakeup pending.
  //
  // The answer has to come from here. Asking afterwards is a different question
  // -- by then the consumer may have drained the queue, and the producer would
  // conclude it need not signal precisely when it must.
  bool push(Node* node) {
    node->next.store(nullptr, std::memory_order_relaxed);

    // release: everything written into the node -- including value -- must be
    // visible to the consumer before the node itself is. The consumer's
    // acquire on next (in pop) is the other half of this pair.
    Node* previous = tail_.exchange(node, std::memory_order_acq_rel);

    // Between the exchange above and the store below, the queue looks empty to
    // the consumer even though this node is already claimed. That window is why
    // pop() can return nullptr with work outstanding, and why a consumer must
    // treat an empty result as "not yet" rather than "never".
    previous->next.store(node, std::memory_order_release);

    // The queue was empty exactly when the node we displaced was the stub and
    // the consumer had already caught up to it.
    return previous == &stub_;
  }

  // Returns nullptr when there is nothing to take. The caller owns what it gets
  // and must delete it.
  //
  // `busy`, when given, distinguishes the two reasons for nullptr, and the
  // difference is the whole ballgame:
  //
  //   busy == false  the queue is genuinely empty. A later push will signal.
  //   busy == true   a producer has claimed its place but not yet linked
  //                  itself in. There IS work here. A consumer that treats this
  //                  as empty and goes back to sleep may never be woken --
  //                  the producer in the window already decided whether to
  //                  signal, and a producer behind it saw a non-empty queue and
  //                  decided not to. Both nodes then sit unread forever.
  //
  // That failure was found by a one-in-twelve test timeout, not by inspection
  // and not by ThreadSanitizer: there is no race here, only a consumer asleep
  // beside a queue with something in it.
  Node* pop(bool* busy = nullptr) {
    if (busy != nullptr) *busy = false;

    Node* head = head_.load(std::memory_order_relaxed);  // consumer-only
    Node* next = head->next.load(std::memory_order_acquire);

    if (head == &stub_) {
      if (next == nullptr) {
        // Empty of values -- but if the tail has moved off the stub, a producer
        // is mid-push and this is the transient case, not the final one.
        if (busy != nullptr && tail_.load(std::memory_order_acquire) != &stub_) {
          *busy = true;
        }
        return nullptr;
      }
      // Step over the stub: it is a fixed marker, not a value.
      head_.store(next, std::memory_order_relaxed);
      head = next;
      next = head->next.load(std::memory_order_acquire);
    }

    if (next != nullptr) {
      head_.store(next, std::memory_order_relaxed);
      return head;
    }

    // head is the last node linked so far. If it is still the tail, the queue is
    // about to be empty: put the stub back so the list is never left headless.
    Node* tail = tail_.load(std::memory_order_acquire);
    if (head != tail) {
      // A producer has swapped in behind head but not yet linked. head is real
      // work that cannot be handed over until that link lands.
      if (busy != nullptr) *busy = true;
      return nullptr;
    }

    push(&stub_);

    next = head->next.load(std::memory_order_acquire);
    if (next != nullptr) {
      head_.store(next, std::memory_order_relaxed);
      return head;
    }
    if (busy != nullptr) *busy = true;
    return nullptr;
  }

 private:
  Node stub_;
  std::atomic<Node*> head_;
  std::atomic<Node*> tail_;
};

}  // namespace shardkv
