// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// A scripted stand-in for the external GPU Cuckatoo solver, for the subprocess
// bridge tests in gpu_solver_tests.cpp.
//
// It is a real compiled executable rather than a shell script for two reasons.
// The first is the one solverprobestub.cpp and lockdirstub.cpp already give:
// "executable" is spelled differently per platform, and CreateProcess cannot
// launch a .bat or .cmd. The second is specific to this bridge: it spawns the
// solver with a real argv and no shell, precisely so that a solver path
// containing a space or a shell metacharacter is passed through untouched. A
// script fixture would need an interpreter in front of it, which is the very
// thing the bridge no longer provides.
//
// Behaviour comes from a directive file named by QS_SOLVER_STUB_SCRIPT rather
// than from the command line, because the bridge owns the command line: it
// always invokes the solver as `<path> <edgebits> <hex> <nonce> <attempts>`.
// One directive per line:
//
//   echo <text>     write <text> and a newline to stdout, flushed immediately
//   sleep <ms>      sleep for <ms> milliseconds
//   append <path>   append one byte to <path>, so a parent can watch a process
//                   that writes nothing to stdout still be alive
//   loop            jump back to the first directive (runs forever)
//   exit <code>     exit with <code>
//
// Running off the end exits 0. Setting QS_SOLVER_STUB_ARGV additionally writes
// the arguments the stub actually received, one per line, so a test can assert
// what the bridge delivered rather than infer it.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
//! Loud and unique: a stub that could not read its own script must not be
//! mistaken for a solver reporting something about a graph. No test expects it.
constexpr int kMisconfigured{70};

//! Locale-free decimal parse, stopping at the first non-digit. The C library's
//! integer-parsing helpers all honour LC_NUMERIC and are banned tree-wide
//! (test/lint/lint-locale-dependence.py); a stub reading its own directives has
//! no business consulting a locale to do it.
int ParseInt(const std::string& text)
{
    int value{0};
    for (const char c : text) {
        if (c < '0' || c > '9') break;
        value = value * 10 + (c - '0');
    }
    return value;
}

std::vector<std::string> ReadLines(const char* path)
{
    std::vector<std::string> lines;
    std::ifstream in{path};
    if (!in) return lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

//! True and sets `rest` when `line` is `<verb> <rest>`.
bool Directive(const std::string& line, const char* verb, std::string& rest)
{
    const size_t n{std::strlen(verb)};
    if (line.size() < n || line.compare(0, n, verb) != 0) return false;
    if (line.size() == n) { rest.clear(); return true; }
    if (line[n] != ' ') return false;
    rest = line.substr(n + 1);
    return true;
}
} // namespace

int main(int argc, char* argv[])
{
    const char* script{std::getenv("QS_SOLVER_STUB_SCRIPT")};
    if (!script || script[0] == '\0') {
        std::cerr << "solver stub: QS_SOLVER_STUB_SCRIPT is unset" << std::endl;
        return kMisconfigured;
    }

    if (const char* argv_out{std::getenv("QS_SOLVER_STUB_ARGV")}) {
        std::ofstream out{argv_out, std::ios::binary};
        for (int i = 1; i < argc; ++i) out << argv[i] << "\n";
    }

    const std::vector<std::string> lines{ReadLines(script)};
    if (lines.empty()) {
        std::cerr << "solver stub: no directives in " << script << std::endl;
        return kMisconfigured;
    }

    for (size_t i{0}; i < lines.size();) {
        const std::string& line{lines[i]};
        std::string rest;
        if (Directive(line, "echo", rest)) {
            // Flushed per line: the bridge's pipe-drain loop is what is under
            // test, and a stub that buffered its output until exit would never
            // exercise it.
            std::fputs(rest.c_str(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
        } else if (Directive(line, "sleep", rest)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ParseInt(rest)));
        } else if (Directive(line, "append", rest)) {
            std::ofstream marker{rest, std::ios::binary | std::ios::app};
            marker << "x";
        } else if (Directive(line, "loop", rest)) {
            i = 0;
            continue;
        } else if (Directive(line, "exit", rest)) {
            return ParseInt(rest);
        } else {
            std::cerr << "solver stub: unknown directive '" << line << "'" << std::endl;
            return kMisconfigured;
        }
        ++i;
    }
    return 0;
}
