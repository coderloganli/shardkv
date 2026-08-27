#pragma once

#include <chrono>

namespace shardkv {

using TimePoint = std::chrono::steady_clock::time_point;

// Time is reached through this rather than called directly, so that expiry can
// be tested by moving the clock instead of by sleeping. A test that sleeps is
// slow when it passes and flaky when it does not.
//
// The sampled half of expiry, which arrives with the later resource-management
// work, needs the same seam.
class Clock {
 public:
  virtual ~Clock() = default;
  virtual TimePoint now() const = 0;
};

// The real one: steady_clock, which does not go backwards when the system time
// is adjusted.
class SteadyClock final : public Clock {
 public:
  TimePoint now() const override { return std::chrono::steady_clock::now(); }
};

// Moves only when told to.
class ManualClock final : public Clock {
 public:
  TimePoint now() const override { return now_; }
  void advance(std::chrono::nanoseconds by) { now_ += by; }

 private:
  TimePoint now_ = TimePoint{} + std::chrono::hours(1);
};

}  // namespace shardkv
