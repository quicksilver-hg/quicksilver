// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// A scripted stand-in for tor, for the bundled-Tor tests in
// bundled_tor_tests.cpp. It is a real compiled executable rather than a script
// for the reason solverstub.cpp and lockdirstub.cpp already give: "executable"
// is spelled differently per platform, CreateProcess cannot launch a .bat or
// .cmd, and the supervisor spawns with a real argv and no shell -- so a script
// fixture would need an interpreter in front of it, which is the very thing
// that is no longer there.
//
// It reads the torrc it was given with `-f`, exactly as tor would, which is what
// makes the generated torrc itself part of what these tests exercise. Two of its
// directives are honoured:
//
//   ControlPortWriteToFile <path>   where the `port` directive writes
//   __OwningControllerProcess <pid> exit as soon as that process is gone
//
// The second is not decoration. On POSIX a process group does not die with the
// process that created it, so the supervisor's promise that no Tor outlives the
// node rests on this mechanism; a stub that ignored it would let the orphan case
// pass while the real thing leaked a Tor with our onion service still published.
//
// Behaviour comes from a directive file named by QS_TOR_STUB_SCRIPT rather than
// from the command line, because the supervisor owns the command line: it always
// invokes tor as `<path> -f <torrc>`. One directive per line:
//
//   port <text>     write <text> to the ControlPortWriteToFile path, with the
//                   two characters \n meaning a newline (a directive file is
//                   line-based, so the newline cannot be written literally)
//   sleep <ms>      sleep for <ms> milliseconds
//   append <path>   append one byte to <path>, so a parent can watch a process
//                   that writes nothing at all still be alive
//   loop            jump back to the first directive (runs forever)
//   exit <code>     exit with <code>
//
// Running off the end exits 0.

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef WIN32
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {
//! Loud and unique: a stub that could not read its own script or its torrc must
//! not be mistaken for a tor reporting something about the network. No test
//! expects it.
constexpr int kMisconfigured{70};

//! Locale-free decimal parse, stopping at the first non-digit. The C library's
//! integer-parsing helpers all honour LC_NUMERIC and are banned tree-wide
//! (test/lint/lint-locale-dependence.py); a stub reading its own directives has
//! no business consulting a locale to do it.
long ParseLong(const std::string& text)
{
    long value{0};
    for (const char c : text) {
        if (c < '0' || c > '9') break;
        value = value * 10 + (c - '0');
    }
    return value;
}

std::vector<std::string> ReadLines(const std::string& path)
{
    std::vector<std::string> lines;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

//! The value of `key` in the torrc, or an empty string.
std::string TorrcValue(const std::vector<std::string>& torrc, const std::string& key)
{
    for (const std::string& line : torrc) {
        if (line.rfind(key + " ", 0) != 0) continue;
        return line.substr(key.size() + 1);
    }
    return {};
}

//! Turn the two characters \n into a real newline. A directive file is
//! line-based, so a test that needs to write a complete line -- or deliberately
//! to write an incomplete one -- has to spell the terminator somehow.
std::string Unescape(const std::string& text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'n') {
            out += '\n';
            ++i;
            continue;
        }
        out += text[i];
    }
    return out;
}

//! True while `pid` is a live process. Advisory on both platforms -- this is a
//! test fixture, not a supervisor -- but faithful to what tor does with
//! __OwningControllerProcess.
bool OwnerAlive(long pid)
{
    if (pid <= 0) return true;
#ifdef WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
#else
    if (kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno == EPERM;
#endif
}
} // namespace

int main(int argc, char* argv[])
{
    std::string torrc_path;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "-f") torrc_path = argv[i + 1];
    }
    if (torrc_path.empty()) return kMisconfigured;

    const std::vector<std::string> torrc = ReadLines(torrc_path);
    if (torrc.empty()) return kMisconfigured;
    const std::string port_file = TorrcValue(torrc, "ControlPortWriteToFile");
    const long owner_pid = ParseLong(TorrcValue(torrc, "__OwningControllerProcess"));

    const char* script = std::getenv("QS_TOR_STUB_SCRIPT");
    if (!script || !script[0]) return kMisconfigured;
    const std::vector<std::string> directives = ReadLines(script);
    if (directives.empty()) return kMisconfigured;

    for (size_t i = 0; i < directives.size(); ++i) {
        if (!OwnerAlive(owner_pid)) return 0;
        const std::string& line = directives[i];
        const size_t sp = line.find(' ');
        const std::string verb = line.substr(0, sp);
        const std::string rest = (sp == std::string::npos) ? std::string{} : line.substr(sp + 1);

        if (verb == "port") {
            if (port_file.empty()) return kMisconfigured;
            std::ofstream f(port_file, std::ios::binary | std::ios::trunc);
            f << Unescape(rest);
            f.flush();
        } else if (verb == "sleep") {
            std::this_thread::sleep_for(std::chrono::milliseconds(ParseLong(rest)));
        } else if (verb == "append") {
            std::ofstream f(rest, std::ios::binary | std::ios::app);
            f << 'x';
            f.flush();
        } else if (verb == "loop") {
            i = static_cast<size_t>(-1);   // ++i restarts at 0
        } else if (verb == "exit") {
            return static_cast<int>(ParseLong(rest));
        } else if (!verb.empty()) {
            return kMisconfigured;
        }
    }
    return 0;
}
