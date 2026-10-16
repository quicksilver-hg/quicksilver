// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// Test helper: --list-devices copies QS_PROBE_STUB_LISTING after an optional
// delay and returns QS_PROBE_STUB_EXIT. Other invocations succeed unchanged.

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
int EnvInt(const char* name)
{
    const char* v = std::getenv(name);
    int n = 0;
    for (; v && *v >= '0' && *v <= '9'; ++v) n = n * 10 + (*v - '0');
    return n;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--list-devices") == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(EnvInt("QS_PROBE_STUB_SLEEP_MS")));
        if (const char* listing = std::getenv("QS_PROBE_STUB_LISTING")) {
            std::ifstream in(listing);
            std::cout << in.rdbuf();
            std::cout.flush();
        }
        return EnvInt("QS_PROBE_STUB_EXIT");
    }
    return 0;
}
