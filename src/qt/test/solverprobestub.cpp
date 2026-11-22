// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// Test helper: --list-devices copies QS_PROBE_STUB_LISTING after an optional
// delay and returns QS_PROBE_STUB_EXIT. Other invocations succeed unchanged.
//
// A value that is not a whole number from 0 to INT_MAX exits 97, and a listing
// that is set but cannot be read exits 98, each with a line on stderr, so a
// broken fixture never passes for a real exit code or an empty device list.

#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
#include <utility>

namespace {
constexpr int EXIT_BAD_VALUE{97};
constexpr int EXIT_UNREADABLE_LISTING{98};

//! Unset or empty reads as 0. Anything else must be all decimal digits and fit an int.
bool EnvInt(const char* name, int& out)
{
    out = 0;
    const char* v = std::getenv(name);
    if (!v || !*v) return true;
    long long n{0};
    for (const char* p = v; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        n = n * 10 + (*p - '0');
        if (n > INT_MAX) return false;
    }
    out = static_cast<int>(n);
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--list-devices") == 0) {
        int sleep_ms{0};
        int exit_code{0};
        for (const auto& [name, value] : {std::pair{"QS_PROBE_STUB_SLEEP_MS", &sleep_ms}, std::pair{"QS_PROBE_STUB_EXIT", &exit_code}}) {
            if (!EnvInt(name, *value)) {
                std::cerr << "test_solver_probe_stub: " << name << " is not a whole number from 0 to " << INT_MAX << ": " << std::getenv(name) << '\n';
                return EXIT_BAD_VALUE;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        if (const char* listing = std::getenv("QS_PROBE_STUB_LISTING")) {
            std::ifstream in(listing);
            if (!in) {
                std::cerr << "test_solver_probe_stub: cannot read QS_PROBE_STUB_LISTING " << listing << '\n';
                return EXIT_UNREADABLE_LISTING;
            }
            std::cout << in.rdbuf();
            std::cout.flush();
        }
        return exit_code;
    }
    return 0;
}
