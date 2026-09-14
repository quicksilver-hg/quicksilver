// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Calibration harness for the transaction-cost flag day. Searches N graphs at a
// given EDGEBITS and prints one CSV row per graph: how long it took and whether
// it held a 42-cycle. M1 (time per graph) and M2 (cycle rate) both come from
// this output. Mining-class code — never on a validation path.
//
//   qscalibrate --edgebits=25 --graphs=200 --threads=4 > run.csv
//
// One row per graph rather than a summary: a summary would discard the
// distribution, and raw rows let a later reader recompute a statistic we did not
// think to record.

#include <crypto/cuckatoo/bench/gpu_csv.h>
#include <crypto/cuckatoo/bench/registry.h>
#include <crypto/cuckatoo/cuckatoo.h>
#include <util/string.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

namespace {

enum class ParseResult { NotThisFlag, Ok, Malformed };

//! Parse "--name=value" into `out`. Reports Malformed if the flag is present but
//! its value is not a plain unsigned integer, so a typo becomes an error rather
//! than a silent default. A sweep run under the wrong parameters is worse than
//! one that did not run.
ParseResult ParseUInt(std::string_view arg, std::string_view name, unsigned& out, bool& seen)
{
    const std::string prefix = std::string("--") + std::string(name) + "=";
    if (arg.substr(0, prefix.size()) != prefix) return ParseResult::NotThisFlag;
    const std::string_view value = arg.substr(prefix.size());
    unsigned parsed = 0;
    const auto* end = value.data() + value.size();
    const auto res = std::from_chars(value.data(), end, parsed);
    if (res.ec != std::errc{} || res.ptr != end) return ParseResult::Malformed;
    out = parsed;
    seen = true;
    return ParseResult::Ok;
}

//! Write `nonce` as the trailing 4 little-endian bytes, exactly as solve_one does
//! internally. verify_one needs the pre-image the graph was actually keyed by.
std::vector<unsigned char> KeyedPrepow(const std::vector<unsigned char>& prepow,
                                       uint32_t nonce)
{
    std::vector<unsigned char> buf = prepow;
    const size_t len = buf.size();
    buf[len - 4] = (unsigned char)(nonce & 0xff);
    buf[len - 3] = (unsigned char)((nonce >> 8) & 0xff);
    buf[len - 2] = (unsigned char)((nonce >> 16) & 0xff);
    buf[len - 1] = (unsigned char)((nonce >> 24) & 0xff);
    return buf;
}

} // namespace

//! Re-check a qsgpucalibrate CSV on the CPU. Returns a process exit code.
int VerifyGpuCsvFile(const std::string& path)
{
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "qscalibrate: cannot open %s\n", path.c_str());
        return 1;
    }
    // The pre-image both harnesses are fixed to. qsgpucalibrate.cu carries the
    // same literal; they must not drift apart.
    std::vector<unsigned char> prepow(84, 0);  // production block pre-pow width
    const char* seed = "quicksilver-calibration-vector";
    std::memcpy(prepow.data(), seed, std::strlen(seed));

    const auto v = cuckatoo::bench::VerifyGpuCsv(in, prepow);
    if (!v.error.empty()) {
        std::fprintf(stderr, "qscalibrate: %s: %s\n", path.c_str(), v.error.c_str());
        return 5;
    }
    const std::string summary = path + ": rows=" + util::ToString(v.rows) +
                                " found=" + util::ToString(v.found) +
                                " verified=" + util::ToString(v.verified) +
                                " consensus_checked=" + util::ToString(v.consensus_checked) +
                                " failed=" + util::ToString(v.failed_nonces.size()) + "\n";
    std::fputs(summary.c_str(), stdout);
    if (!v.failed_nonces.empty()) {
        std::fprintf(stderr, "qscalibrate: %zu cycle(s) FAILED CPU verification; "
                             "first offending nonces:", v.failed_nonces.size());
        for (size_t i = 0; i < v.failed_nonces.size() && i < 20; ++i) {
            std::fprintf(stderr, " %u", v.failed_nonces[i]);
        }
        std::fprintf(stderr, "\n");
        return 4;
    }
    return 0;
}

//! Six fixed decimal places, locale-independently. printf's "%.6f" honours
//! the numeric locale, so under a comma-decimal one it emits "1,234567" and silently
//! corrupts the CSV this tool exists to produce. The measurement is a duration, so
//! integer microseconds are exact and no rounding decision is hidden.
static std::string Seconds6(double secs)
{
    if (secs < 0) secs = 0;
    const uint64_t us = (uint64_t)(secs * 1e6 + 0.5);
    std::string frac = util::ToString(us % 1000000);
    frac.insert(0, 6 - frac.size(), '0');
    return util::ToString(us / 1000000) + "." + frac;
}

int main(int argc, char* argv[])
{
    unsigned edgebits = 0, graphs = 0, threads = 0;
    bool have_edgebits = false, have_graphs = false, have_threads = false;

    // Verification mode is a different job from timing: it consumes a GPU run's
    // output instead of producing one, so it takes no other flags.
    const std::string_view kVerifyFlag{"--verify-gpu-csv="};
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg.substr(0, kVerifyFlag.size()) == kVerifyFlag) {
            if (argc != 2) {
                std::fprintf(stderr,
                             "qscalibrate: --verify-gpu-csv takes no other arguments\n");
                return 1;
            }
            return VerifyGpuCsvFile(std::string(arg.substr(kVerifyFlag.size())));
        }
    }

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        bool matched = false;
        for (const auto& [name, out, seen] :
             {std::tuple{std::string_view{"edgebits"}, &edgebits, &have_edgebits},
              std::tuple{std::string_view{"graphs"}, &graphs, &have_graphs},
              std::tuple{std::string_view{"threads"}, &threads, &have_threads}}) {
            const ParseResult res = ParseUInt(arg, name, *out, *seen);
            if (res == ParseResult::Malformed) {
                std::fprintf(stderr, "qscalibrate: malformed argument: %s\n", argv[i]);
                return 1;
            }
            if (res == ParseResult::Ok) {
                matched = true;
                break;
            }
        }
        // An argument matching no known flag is a typo too — accepting it
        // silently would let a mistyped --thread=8 run at the default width.
        if (!matched) {
            std::fprintf(stderr, "qscalibrate: unknown argument: %s\n", argv[i]);
            return 1;
        }
    }

    if (!have_edgebits || !have_graphs) {
        std::fprintf(stderr,
                     "usage: qscalibrate --edgebits=N --graphs=N [--threads=N]\n"
                     "       qscalibrate --verify-gpu-csv=PATH\n");
        return 1;
    }
    if (edgebits > 255) {
        std::fprintf(stderr, "qscalibrate: edgebits out of range\n");
        return 1;
    }
    // Refuse to emit a header-only file: downstream that reads as "this size
    // found no cycles" rather than "this size was never measured".
    if (graphs == 0) {
        std::fprintf(stderr, "qscalibrate: --graphs must be at least 1\n");
        return 1;
    }
    if (!have_threads || threads == 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        threads = hw == 0 ? 1 : hw;
    }

    const auto* vt = cuckatoo::bench::Lookup(static_cast<uint8_t>(edgebits));
    if (vt == nullptr) {
        std::fprintf(stderr, "qscalibrate: no bench solver for edgebits %u\n", edgebits);
        return 2;
    }

    // A fixed, documented pre-image so a run is reproducible from its parameters.
    std::vector<unsigned char> prepow(84, 0);  // production block pre-pow width
    const char* seed = "quicksilver-calibration-vector";
    std::memcpy(prepow.data(), seed, std::strlen(seed));

    void* ctx = vt->create(threads);
    if (ctx == nullptr) {
        std::fprintf(stderr, "qscalibrate: solver context allocation failed\n");
        return 3;
    }

    std::fputs("edgebits,nonce,seconds,found,threads\n", stdout);
    for (unsigned n = 0; n < graphs; ++n) {
        cuckatoo::Cycle cyc{};
        const auto t0 = std::chrono::steady_clock::now();
        const bool found = vt->solve_one(ctx, prepow.data(), prepow.size(), n, cyc);
        const auto t1 = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(t1 - t0).count();
        // A cycle that does not verify at its own graph size means the solver is
        // broken; recording the timing anyway would put a worthless number into
        // the sweep. Fail the run instead.
        const std::vector<unsigned char> keyed = KeyedPrepow(prepow, n);
        if (found && !vt->verify_one(cyc, keyed.data(), keyed.size())) {
            std::fprintf(stderr,
                         "qscalibrate: solver reported an INVALID cycle at edgebits %u "
                         "nonce %u — aborting; no timing from this run is usable\n",
                         edgebits, n);
            vt->destroy(ctx);
            return 4;
        }
        const std::string row = util::ToString(edgebits) + "," + util::ToString(n) + "," +
                                Seconds6(secs) + "," + (found ? "1" : "0") + "," +
                                util::ToString(threads) + "\n";
        std::fputs(row.c_str(), stdout);
        std::fflush(stdout);  // a killed long run keeps the rows it earned
    }

    vt->destroy(ctx);
    return 0;
}
