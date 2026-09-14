#pragma once
#include <condition_variable>
#include <mutex>

// Quicksilver: ported from the vendored pthread barrier to std threading primitives
// so the CPU lean solver builds under MSVC (no pthreads). Semantics are preserved
// exactly: wait() blocks until all `limit` threads arrive, then advances the phase.
// On abort, wait() returns true and the worker terminates cooperatively (the pthread
// build called pthread_exit here — impossible with std::thread). abort() is only wired
// through stop_solver(), which the node's synchronous solve path never invokes, so this
// branch is inert in practice; the cooperative return keeps it faithful regardless.
// Mirrors the Linux build, which compiles this solver unconditionally.
class trim_barrier {
  std::mutex mtx;
  std::condition_variable cond;
  unsigned limit;
  unsigned count;
  int phase;

public:
  trim_barrier(unsigned int count_) : limit(count_), count(0), phase(0) {}

  void clear() {
    count = phase = 0;
  }

  void abort() {
    std::lock_guard<std::mutex> lk(mtx);
    phase = -1;
  }

  bool aborted() {
    return phase < 0;
  }

  // Returns true if the barrier was aborted (the caller should stop).
  bool wait() {
    std::unique_lock<std::mutex> lk(mtx);
    int wait_phase = phase;
    if (++count >= limit) {
      if (wait_phase >= 0) {
        phase = wait_phase + 1;
        count = 0;
      }
      cond.notify_all();
    } else if (wait_phase >= 0) {
      cond.wait(lk, [&]{ return phase != wait_phase; });
    }
    return wait_phase < 0;
  }
};
