#pragma once
#include <chrono>
#include <cstdio>
#include <sys/resource.h>

namespace bench {
using clk = std::chrono::steady_clock;
inline double ms_since(clk::time_point t0) {
  return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}
inline double us_since(clk::time_point t0) {
  return std::chrono::duration<double, std::micro>(clk::now() - t0).count();
}
inline long peak_rss_kb() {
  struct rusage ru; getrusage(RUSAGE_SELF, &ru); return ru.ru_maxrss;
}
inline void emit_row(const char* algorithm, const char* param_label,
                     const char* hardware_label, int run_index,
                     double create_ms, double verify_us, long proof_bytes,
                     long verify_peak_kb, long create_peak_kb) {
  std::printf("%s,%s,%s,%d,%.4f,%.4f,%ld,%ld,%ld\n", algorithm, param_label,
              hardware_label, run_index, create_ms, verify_us, proof_bytes,
              verify_peak_kb, create_peak_kb);
}
}  // namespace bench
