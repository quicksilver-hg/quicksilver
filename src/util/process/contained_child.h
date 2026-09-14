// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// One child process, contained: spawned from a real argument vector with no
// shell, held in a kill-as-a-unit group (a job object on Windows, a process
// group on POSIX), and killable along with anything it started.
//
// Extracted from the Cuckatoo GPU-solver bridge, where every line of it was
// written to close a defect that had already reached a live node: a _pclose
// waiting on a child that could never exit, a shell that turned the solver into
// an unreachable grandchild, and a containment step whose failure was ignored
// and reported as a clean stop. It is shared rather than copied because a second
// implementation of this is a second chance to reintroduce those.
//
// This unit owns CONTAINMENT, not lifecycle. The solver waits on a short-lived
// child; bundled Tor supervises a long-lived one. Neither policy lives here.
#ifndef QUICKSILVER_UTIL_PROCESS_CONTAINED_CHILD_H
#define QUICKSILVER_UTIL_PROCESS_CONTAINED_CHILD_H

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace util {

/**
 * Join `argv` into a single Windows command line, quoted so that the child's
 * C runtime splits it back into exactly these arguments (the CommandLineToArgvW
 * rules). CreateProcess takes one string, not a vector, so on that platform this
 * quoting *is* the argv: an unquoted space turns one path into two arguments and
 * an undoubled trailing backslash escapes the closing quote.
 *
 * Declared here, rather than kept private to the WIN32 branch of the .cpp, so
 * that it is compiled and tested on every platform. The rules are pure string
 * manipulation and owe nothing to the OS; confining them to the one platform
 * this project cannot run in CI is how the previous Windows bridge shipped a
 * deadlock.
 */
std::string WindowsCommandLine(const std::vector<std::string>& argv);

//! What the parent does with the child's standard output.
enum class ChildStdout {
    //! Readable through ReadStdout(). The parent MUST drain it: a child that
    //! fills the pipe buffer blocks in write() forever, which is exactly how the
    //! old solver bridge deadlocked with the GPU pinned.
    kPipe,
    //! Sent to the null device. The right choice for a long-lived child that
    //! logs to a file of its own and whose output nobody is reading.
    kDiscard,
};

//! Fault seam for the resource-failure paths. pipe()/fork()/CreateProcess and
//! the containment primitives fail only under resource exhaustion, so without a
//! seam their handling is unreachable from a test and ships unexecuted. That is
//! precisely how the old Windows bridge shipped a deadlock. Strictly weaker than
//! the environment variables the callers already honour: anyone who can set the
//! node's environment can already name the program it runs.
enum class SpawnFault {
    kNone,
    kPipe,         //!< the stdout pipe cannot be created
    kSpawn,        //!< fork()/CreateProcess() fails
    kContainment,  //!< the process group / job object cannot be established
};

//! Read a SpawnFault from an environment variable ("pipe"/"spawn"/
//! "containment"); anything else, including an unset variable, is kNone. The
//! variable's NAME belongs to the caller, so each bridge keeps its own and this
//! unit gains no configuration of its own.
SpawnFault SpawnFaultFromEnv(const char* env_var);

class ContainedChild
{
public:
    enum class ReadStatus {
        kData,     //!< `got` bytes were read
        kTimeout,  //!< the child was silent for the whole timeout and is still running
        kClosed,   //!< end of stream: the child exited, or the pipe failed
    };

    //! Spawn `argv` (argv[0] is the program) contained. Returns nullptr if the
    //! child could not be started AND contained -- in which case nothing is
    //! left running: a child that cannot be contained is never allowed to exec.
    static std::unique_ptr<ContainedChild> Spawn(const std::vector<std::string>& argv,
                                                 ChildStdout stdout_mode,
                                                 SpawnFault inject = SpawnFault::kNone);

    //! Kills the group and reaps, if the caller has not already.
    ~ContainedChild();

    ContainedChild(const ContainedChild&) = delete;
    ContainedChild& operator=(const ContainedChild&) = delete;

    //! Wait up to `timeout_ms` for output. Always kClosed for a kDiscard child.
    ReadStatus ReadStdout(char* buf, size_t buflen, int timeout_ms, size_t& got);

    //! True once the child has exited. Does not block. Reaps on POSIX so that a
    //! finished child does not linger as a zombie while a supervisor polls.
    bool Exited();

    //! Kill the child and everything it started, as a unit. Idempotent.
    void KillGroup();

    //! Block until the child is gone and return its exit code, or -1 if it did
    //! not exit normally (killed, signalled, or already reaped as such).
    int Wait();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    explicit ContainedChild(std::unique_ptr<Impl> impl);
};

} // namespace util

#endif // QUICKSILVER_UTIL_PROCESS_CONTAINED_CHILD_H
