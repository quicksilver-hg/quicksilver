// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/cuckatoo/bench/gpu_csv.h>

#include <crypto/cuckatoo/bench/registry.h>
#include <crypto/cuckatoo/cuckatoo.h>
#include <util/string.h>

#include <charconv>
#include <cstdint>
#include <istream>
#include <sstream>

namespace cuckatoo {
namespace bench {

namespace {

//! The exact schema qsgpucalibrate emits. Checked rather than assumed: the CPU
//! sweep's files share the first four column names, so a mixed-up path would
//! otherwise be read as GPU output with `threads` silently reinterpreted.
const char* kExpectedHeader = "edgebits,nonce,seconds,found,device,cycle";

//! Place `nonce` in the trailing 4 little-endian bytes, as both solvers do.
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

bool ParseU32(const std::string& s, unsigned base, uint32_t& out)
{
    if (s.empty()) return false;
    // std::from_chars rather than std::stoull: it is locale-independent by
    // definition, takes the base directly, and reports how much it consumed
    // without throwing. A CSV parser that reads different numbers under a
    // different numeric locale is exactly the class of bug the locale lint exists
    // to stop, and this file parses recorded calibration measurements.
    uint64_t v = 0;
    const char* first = s.data();
    const char* last = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(first, last, v, static_cast<int>(base));
    if (ec != std::errc{} || ptr != last) return false;
    if (v > 0xffffffffULL) return false;
    out = (uint32_t)v;
    return true;
}

//! Split a line on commas. The cycle field holds space-separated values and
//! never a comma, so a plain split is exact for this schema.
std::vector<std::string> SplitCsv(const std::string& line)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t comma = line.find(',', start);
        if (comma == std::string::npos) {
            out.push_back(line.substr(start));
            return out;
        }
        out.push_back(line.substr(start, comma - start));
        start = comma + 1;
    }
}

//! Parse exactly PROOFSIZE space-separated hex indices. Anything else fails:
//! a short proof must never be zero-padded into something that might verify.
bool ParseCycle(const std::string& field, Cycle& out)
{
    std::istringstream is(field);
    std::string tok;
    int n = 0;
    while (is >> tok) {
        if (n >= PROOFSIZE) return false;
        uint32_t v = 0;
        if (!ParseU32(tok, 16, v)) return false;
        out[n++] = v;
    }
    return n == PROOFSIZE;
}

std::string Trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\r' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\r' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

} // namespace

GpuCsvVerdict VerifyGpuCsv(std::istream& in, const std::vector<unsigned char>& prepow)
{
    GpuCsvVerdict v;

    if (prepow.size() < 4) {
        v.error = "pre-image shorter than the 4-byte nonce tail";
        return v;
    }

    std::string line;
    if (!std::getline(in, line)) {
        v.error = "empty input: no header row";
        return v;
    }
    if (Trim(line) != kExpectedHeader) {
        v.error = "unexpected header: got \"" + Trim(line) + "\", want \"" +
                  kExpectedHeader + "\"";
        return v;
    }

    size_t lineno = 1;
    while (std::getline(in, line)) {
        ++lineno;
        if (Trim(line).empty()) continue;

        const std::vector<std::string> f = SplitCsv(line);
        if (f.size() != 6) {
            v.error = "line " + util::ToString(lineno) + ": expected 6 fields, got " +
                      util::ToString(f.size());
            return v;
        }

        uint32_t edgebits = 0, nonce = 0, found = 0;
        if (!ParseU32(Trim(f[0]), 10, edgebits) || !ParseU32(Trim(f[1]), 10, nonce) ||
            !ParseU32(Trim(f[3]), 10, found) || found > 1) {
            v.error = "line " + util::ToString(lineno) + ": malformed edgebits/nonce/found";
            return v;
        }
        if (edgebits > 255) {
            v.error = "line " + util::ToString(lineno) + ": edgebits out of range";
            return v;
        }

        ++v.rows;
        const std::string cycle_field = Trim(f[5]);

        if (found == 0) {
            // A cycle on a row that claims none is a producer bug, not a
            // harmless extra: one of the two fields is wrong and we cannot tell
            // which.
            if (!cycle_field.empty()) {
                v.error = "line " + util::ToString(lineno) +
                          ": found=0 but a cycle is present";
                return v;
            }
            continue;
        }

        ++v.found;
        if (cycle_field.empty()) {
            v.error = "line " + util::ToString(lineno) + ": found=1 but no cycle";
            return v;
        }

        const SolverVTable* vt = Lookup((uint8_t)edgebits);
        if (vt == nullptr) {
            // Cannot check this size at all. Returning success here would make
            // "unverified" indistinguishable from "verified" downstream.
            v.error = "line " + util::ToString(lineno) + ": no bench solver for edgebits " +
                      util::ToString(edgebits);
            return v;
        }

        Cycle cyc{};
        if (!ParseCycle(cycle_field, cyc)) {
            v.error = "line " + util::ToString(lineno) + ": cycle is not " +
                      util::ToString(PROOFSIZE) + " hex indices";
            return v;
        }

        const std::vector<unsigned char> keyed = KeyedPrepow(prepow, nonce);
        const bool ok = vt->verify_one(cyc, keyed.data(), keyed.size());

        // Where consensus can speak, let it. This is the tie between the whole
        // calibration harness and the code the network actually runs.
        if (edgebits == 19 || edgebits == 29) {
            const Keys keys = CuckatooSetHeader(keyed.data(), (uint32_t)keyed.size());
            const bool consensus_ok = CuckatooVerify(cyc, keys, (uint8_t)edgebits);
            ++v.consensus_checked;
            if (consensus_ok != ok) {
                v.error = "line " + util::ToString(lineno) +
                          ": bench and consensus verifiers disagree at edgebits " +
                          util::ToString(edgebits);
                return v;
            }
        }

        if (ok) {
            ++v.verified;
        } else {
            v.failed_nonces.push_back(nonce);
        }
    }

    return v;
}

} // namespace bench
} // namespace cuckatoo
