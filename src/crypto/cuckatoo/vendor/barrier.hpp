#pragma once
#include <condition_variable>
#include <mutex>

// Wait until all worker threads arrive, then advance the phase.
class trim_barrier {
  std::mutex mtx;
  std::condition_variable cond;
  unsigned limit;
  unsigned count;
  unsigned phase;

public:
  trim_barrier(unsigned int count_) : limit(count_), count(0), phase(0) {}

  void clear() {
    count = phase = 0;
  }

  void wait() {
    std::unique_lock<std::mutex> lk(mtx);
    const unsigned wait_phase = phase;
    if (++count >= limit) {
      ++phase;
      count = 0;
      cond.notify_all();
    } else {
      cond.wait(lk, [&]{ return phase != wait_phase; });
    }
  }
};
