// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// A scripted stand-in for the external GPU Cuckatoo solver, for the subprocess
// bridge and discovery tests in gpu_solver_tests.cpp and gpu_devices_tests.cpp.
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
//   await <path>    poll until <path> exists, so one card can wait until
//                   another has reached that point. Gives up after 4 s and
//                   exits non-zero, so a marker that never appears cannot
//                   hang the suite
//   loop            jump back to the first directive (runs forever)
//   exit <code>     exit with <code>
//
// QS_SOLVER_STUB_DEVICE_SCRIPTS may name a tab-separated `device<TAB>script`
// map; solve invocations use the matching per-device script. Running off the
// end exits 0. Setting QS_SOLVER_STUB_ARGV additionally writes
// the arguments the stub actually received, one per line, so a test can assert
// what the bridge delivered rather than infer it. QS_SOLVER_STUB_ARGV_APPEND
// appends one tab-separated invocation per spawn, allowing concurrent launches
// to be recorded without changing the original overwrite contract.

#include <util/fs.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#ifdef WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
//! Loud and unique: a stub that could not read its own script must not be
//! mistaken for a solver reporting something about a graph. No test expects it.
constexpr int kMisconfigured{70};

//! Upper bound for `await`. Long enough that a loaded scheduler can still
//! start the card that writes the marker, short enough that a missing marker
//! fails the test instead of hanging it.
constexpr int kAwaitBoundMs{4000};

//! An environment variable as UTF-8, empty when unset. Paths in the stub are UTF-8 throughout: on Windows the
//! narrow getenv goes through the ANSI code page and mangles a path outside it (the functional test runner's
//! folder name carries non-ASCII characters on purpose), so the variable is read wide.
std::string Env(const char* name)
{
#ifdef WIN32
    const wchar_t* value{_wgetenv(std::wstring(name, name + std::strlen(name)).c_str())};
    return value ? fs::path{std::wstring{value}}.utf8string() : std::string{};
#else
    const char* value{std::getenv(name)};
    return value ? std::string{value} : std::string{};
#endif
}

bool PathExists(const std::string& path)
{
#ifdef WIN32
    return GetFileAttributesW(fs::PathFromString(path).wstring().c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    return access(path.c_str(), F_OK) == 0;
#endif
}

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

std::vector<std::string> ReadLines(const std::string& path)
{
    std::vector<std::string> lines;
    std::ifstream in{fs::PathFromString(path)};
    if (!in) return lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

//! True and sets `rest` when `line` is `<verb> <rest>`.
// Locale-independent, like ParseInt: the stub only ever sees decimal nonces.
unsigned long long ParseU64(const char* text)
{
    unsigned long long value{0};
    for (; *text >= '0' && *text <= '9'; ++text) value = value * 10 + static_cast<unsigned>(*text - '0');
    return value;
}

std::string Decimal(unsigned long long value)
{
    std::string out;
    do { out.insert(out.begin(), static_cast<char>('0' + value % 10)); value /= 10; } while (value != 0);
    return out;
}

bool Directive(const std::string& line, const char* verb, std::string& rest)
{
    const size_t n{std::strlen(verb)};
    if (line.size() < n || line.compare(0, n, verb) != 0) return false;
    if (line.size() == n) { rest.clear(); return true; }
    if (line[n] != ' ') return false;
    rest = line.substr(n + 1);
    return true;
}

bool AppendLaunchRecord(const std::string& path, const std::string& record)
{
#ifdef WIN32
    const HANDLE file{CreateFileW(fs::PathFromString(path).wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file == INVALID_HANDLE_VALUE) {
        std::cerr << "solver stub: could not open launch record " << path << std::endl;
        return false;
    }
    bool ok{record.size() <= std::numeric_limits<DWORD>::max()};
    DWORD written{0};
    if (ok) {
        ok = WriteFile(file, record.data(), static_cast<DWORD>(record.size()), &written, nullptr) &&
             written == static_cast<DWORD>(record.size());
    }
    if (!CloseHandle(file)) ok = false;
#else
    const int file{open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644)};
    if (file == -1) {
        std::cerr << "solver stub: could not open launch record " << path << std::endl;
        return false;
    }
    const ssize_t written{write(file, record.data(), record.size())};
    bool ok{written >= 0 && static_cast<size_t>(written) == record.size()};
    if (close(file) != 0) ok = false;
#endif
    if (!ok) {
        std::cerr << "solver stub: could not append complete launch record to " << path << std::endl;
    }
    return ok;
}
} // namespace

int main(int argc, char* argv[])
{
    if (const std::string argv_out{Env("QS_SOLVER_STUB_ARGV")}; !argv_out.empty()) {
        std::ofstream out{fs::PathFromString(argv_out), std::ios::binary};
        for (int i = 1; i < argc; ++i) out << argv[i] << "\n";
    }
    if (const std::string argv_out{Env("QS_SOLVER_STUB_ARGV_APPEND")}; !argv_out.empty()) {
        std::string record;
        for (int i = 1; i < argc; ++i) {
            if (i != 1) record += '\t';
            record += argv[i];
        }
        record += '\n';
        if (!AppendLaunchRecord(argv_out, record)) return kMisconfigured;
    }
    std::string script{Env("QS_SOLVER_STUB_SCRIPT")};
    if (argc == 2 && std::strcmp(argv[1], "--list-devices") == 0) {
        const std::string listing{Env("QS_SOLVER_STUB_LISTING")};
        if (listing.empty()) {
            std::puts("device backend=stub id=pci:0000:01:00.0 type=gpu integrated=0 mem=0 status=ok name=Stub graphics card");
            return 0;
        }
        script = listing;
    }
    if (argc >= 3 && std::strcmp(argv[1], "--device") == 0) {
        if (const std::string map_path{Env("QS_SOLVER_STUB_DEVICE_SCRIPTS")}; !map_path.empty()) {
            const std::vector<std::string> mappings{ReadLines(map_path)};
            const std::string prefix{std::string{argv[2]} + "\t"};
            for (const std::string& mapping : mappings) {
                if (mapping.rfind(prefix, 0) == 0) {
                    script = mapping.substr(prefix.size());
                    break;
                }
            }
        }
    }
    if (script.empty()) {
        std::cerr << "solver stub: QS_SOLVER_STUB_SCRIPT is unset" << std::endl;
        return kMisconfigured;
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
            std::ofstream marker{fs::PathFromString(rest), std::ios::binary | std::ios::app};
            marker << "x";
        } else if (Directive(line, "await", rest)) {
            if (rest.empty()) {
                std::cerr << "solver stub: await needs a path" << std::endl;
                return kMisconfigured;
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{kAwaitBoundMs};
            while (!PathExists(rest)) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    std::cerr << "solver stub: await timed out for " << rest << std::endl;
                    return kMisconfigured;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
        } else if (Directive(line, "found", rest)) {
            // "found NONCE CYCLE-LINE" lines are the proofs this helper can find.
            // At the first one reached it reports the lowest NONCE inside the
            // range this launch was given, and exits as a helper does on its first
            // cycle: a real helper scans upward, so it never skips a lower proof.
            // With none in range it goes on past them. A fixed "echo nonce="
            // stands for a helper that ignores its range.
            if (argc != 7) {
                std::cerr << "solver stub: 'found' needs a solve argv" << std::endl;
                return kMisconfigured;
            }
            // argv: --device ID EDGEBITS PREPOW START ATTEMPTS (gpu_solver.cpp solver_argv).
            const unsigned long long start{ParseU64(argv[5])};
            const unsigned long long attempts{ParseU64(argv[6])};
            const std::string* best{nullptr};
            unsigned long long best_nonce{0};
            size_t next{i};
            for (; next < lines.size(); ++next) {
                std::string proof;
                if (!Directive(lines[next], "found", proof)) break;
                if (proof.find(' ') == std::string::npos) {
                    std::cerr << "solver stub: 'found' needs a nonce and a cycle line" << std::endl;
                    return kMisconfigured;
                }
                const unsigned long long nonce{ParseU64(proof.c_str())};
                if (nonce >= start && nonce - start < attempts && (!best || nonce < best_nonce)) {
                    best = &lines[next];
                    best_nonce = nonce;
                }
            }
            if (best) {
                const std::string reply{"nonce=" + Decimal(best_nonce) + "\n" + best->substr(best->find(' ', 6) + 1) + "\n"};
                std::fputs(reply.c_str(), stdout);
                std::fflush(stdout);
                return 0;
            }
            i = next;
            continue;
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
