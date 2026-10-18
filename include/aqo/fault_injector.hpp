// Deterministic crash injection for tests: a countdown over I/O operations (page writes, log flushes,
// fsyncs). When it reaches zero the operation throws SimulatedCrash, optionally after a partial write.
#pragma once

#include <cstdint>
#include <exception>
#include <random>

namespace aqo {

struct SimulatedCrash : std::exception {
  [[nodiscard]] const char* what() const noexcept override { return "simulated crash"; }
};

class FaultInjector {
 public:
  explicit FaultInjector(std::uint64_t seed = 1) : rng_(seed) {}
  /// The (ops+1)-th I/O operation from now crashes; arm_after(0) crashes the very next one.
  void arm_after(std::int64_t ops) {
    countdown_ = ops;
    fired_ = false;
  }
  void disarm() { countdown_ = -1; }
  [[nodiscard]] bool fired() const { return fired_; }
  [[nodiscard]] bool armed() const { return countdown_ >= 0; }
  /// Called at every I/O point; true means "crash now".
  bool should_crash() {
    if (countdown_ < 0) return false;
    if (countdown_-- == 0) {
      countdown_ = -1;
      fired_ = true;
      return true;
    }
    return false;
  }
  std::uint64_t random_below(std::uint64_t n) { return n == 0 ? 0 : rng_() % n; }

 private:
  std::int64_t countdown_ = -1;
  bool fired_ = false;
  std::mt19937_64 rng_;
};

}  // namespace aqo
