// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <util/process/contained_child.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace util {

std::string WindowsCommandLine(const std::vector<std::string>& argv)
{
    std::string cmdline;
    for (const std::string& arg : argv) {
        if (!cmdline.empty()) cmdline += ' ';
        // Quote only when the splitter would otherwise mis-read the argument.
        // Quoting everything would work too, but an operator reading the command
        // in a log should see the path they configured.
        if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
            cmdline += arg;
            continue;
        }
        cmdline += '"';
        size_t i = 0;
        while (i < arg.size()) {
            size_t backslashes = 0;
            while (i < arg.size() && arg[i] == '\\') { ++backslashes; ++i; }
            if (i == arg.size()) {
                // These backslashes now sit against the closing quote. Undoubled,
                // they would escape it, and everything after this argument --
                // including the next one -- would be swallowed into it.
                cmdline.append(backslashes * 2, '\\');
                break;
            }
            // Backslashes are literal unless they precede a quote, in which case
            // they are halved by the splitter; the quote itself needs one more.
            cmdline.append(arg[i] == '"' ? backslashes * 2 + 1 : backslashes, '\\');
            cmdline += arg[i];
            ++i;
        }
        cmdline += '"';
    }
    return cmdline;
}

SpawnFault SpawnFaultFromEnv(const char* env_var)
{
    const char* f = std::getenv(env_var);
    if (!f) return SpawnFault::kNone;
    if (std::strcmp(f, "pipe") == 0) return SpawnFault::kPipe;
    if (std::strcmp(f, "spawn") == 0) return SpawnFault::kSpawn;
    if (std::strcmp(f, "containment") == 0) return SpawnFault::kContainment;
    return SpawnFault::kNone;
}

} // namespace util

// The per-platform half. Guarded exactly as the solver bridge guarded it: the
// #ifdef closes the namespace, includes the platform headers, and reopens it, so
// that no platform header is pulled into the shared translation unit above.
#ifndef WIN32

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace util {

struct ContainedChild::Impl {
    pid_t pid{-1};
    int stdout_fd{-1};
    bool reaped{false};
    int exit_code{-1};
};

std::unique_ptr<ContainedChild> ContainedChild::Spawn(const std::vector<std::string>& argv,
                                                      ChildStdout stdout_mode,
                                                      SpawnFault inject)
{
    if (argv.empty()) return nullptr;

    // A real argument vector for execvp, not a command string for a shell. It
    // used to be interpolated into `/bin/sh -c`, which made `;` `|` `&` `` ` ``
    // and `$( )` in an operator's path live, and split any path containing a
    // space into separate words. Built here, before the fork, because the child
    // side of a fork in a threaded process may not allocate.
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const std::string& arg : argv) cargv.push_back(const_cast<char*>(arg.c_str()));
    cargv.push_back(nullptr);

    const bool want_pipe = (stdout_mode == ChildStdout::kPipe);
    int fds[2] = {-1, -1};
    if (inject == SpawnFault::kPipe) return nullptr;
    if (want_pipe && pipe(fds) != 0) return nullptr;

    // The POSIX analogue of Windows' CREATE_SUSPENDED. The child blocks here
    // until the parent has CONFIRMED the process group exists; EOF without the
    // go-ahead means the parent could not contain us and the child must never
    // exec, so an uncontainable child is never started rather than started and
    // then unkillable. This mattered most when /bin/sh forked the program into a
    // grandchild, and it still matters: an operator-supplied path is routinely a
    // wrapper, and a wrapper that does not exec puts the real program back out
    // of reach of anything but killpg.
    int sync_fds[2];
    if (pipe(sync_fds) != 0) {
        if (want_pipe) { close(fds[0]); close(fds[1]); }
        return nullptr;
    }

    const pid_t pid = (inject == SpawnFault::kSpawn) ? -1 : fork();
    if (pid < 0) {
        if (want_pipe) { close(fds[0]); close(fds[1]); }
        close(sync_fds[0]); close(sync_fds[1]);
        return nullptr;
    }
    if (pid == 0) {
        // Child: own process group so a runaway (and anything it started) dies
        // as a unit.
        if (inject != SpawnFault::kContainment) setpgid(0, 0);
        close(sync_fds[1]);
        char go = 0;
        if (read(sync_fds[0], &go, 1) != 1 || go != 'g') _exit(126);
        close(sync_fds[0]);
        if (want_pipe) {
            dup2(fds[1], STDOUT_FILENO);
            close(fds[0]); close(fds[1]);
        }
        // Where the shell redirection `2>/dev/null` used to go. Child stderr is
        // diagnostic chatter; the exit code is what carries a fault. If
        // /dev/null cannot be opened, stderr is simply inherited -- noisier,
        // never wrong.
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            if (!want_pipe) dup2(devnull, STDOUT_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        execvp(cargv[0], cargv.data());
        _exit(127);
    }
    close(sync_fds[0]);
    if (inject != SpawnFault::kContainment) setpgid(pid, pid);  // race-free: set in both parent and child
    // Ask the kernel whether the group exists rather than trusting a return
    // value. The parent's setpgid legitimately fails with EACCES once the child
    // has execve'd -- the race the paired call above exists to cover -- so
    // treating its return as the answer would condemn a perfectly contained
    // child. The child is still blocked on the sync pipe here, so this reads the
    // settled state either way.
    const bool pgroup_ok = (inject != SpawnFault::kContainment) && getpgid(pid) == pid;
    if (!pgroup_ok) {
        close(sync_fds[1]);   // EOF: the child aborts before exec, so nothing ever runs
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        if (want_pipe) { close(fds[0]); close(fds[1]); }
        return nullptr;
    }
    const char go = 'g';
    const bool released = write(sync_fds[1], &go, 1) == 1;
    close(sync_fds[1]);
    if (!released) {
        killpg(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        if (want_pipe) { close(fds[0]); close(fds[1]); }
        return nullptr;
    }
    // Past this point the group is an invariant, so the kills below need no guard.
    if (want_pipe) close(fds[1]);

    auto impl = std::make_unique<Impl>();
    impl->pid = pid;
    impl->stdout_fd = want_pipe ? fds[0] : -1;
    return std::unique_ptr<ContainedChild>(new ContainedChild(std::move(impl)));
}

ContainedChild::ReadStatus ContainedChild::ReadStdout(char* buf, size_t buflen, int timeout_ms, size_t& got)
{
    got = 0;
    if (m_impl->stdout_fd < 0) return ReadStatus::kClosed;
    for (;;) {
        struct pollfd pfd; pfd.fd = m_impl->stdout_fd; pfd.events = POLLIN;
        const int pr = poll(&pfd, 1, timeout_ms);
        if (pr < 0) { if (errno == EINTR) continue; return ReadStatus::kClosed; }
        if (pr == 0) return ReadStatus::kTimeout;
        const ssize_t r = read(m_impl->stdout_fd, buf, buflen);
        if (r < 0) { if (errno == EINTR) continue; return ReadStatus::kClosed; }
        if (r == 0) return ReadStatus::kClosed;
        got = static_cast<size_t>(r);
        return ReadStatus::kData;
    }
}

bool ContainedChild::Exited()
{
    if (m_impl->reaped) return true;
    int wstatus = 0;
    const pid_t r = waitpid(m_impl->pid, &wstatus, WNOHANG);
    if (r != m_impl->pid) return false;
    m_impl->reaped = true;
    m_impl->exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
    return true;
}

void ContainedChild::KillGroup()
{
    if (m_impl->reaped) return;
    killpg(m_impl->pid, SIGKILL);
}

int ContainedChild::Wait()
{
    if (m_impl->reaped) return m_impl->exit_code;
    int wstatus = 0;
    waitpid(m_impl->pid, &wstatus, 0);
    m_impl->reaped = true;
    m_impl->exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
    return m_impl->exit_code;
}

ContainedChild::~ContainedChild()
{
    if (m_impl->stdout_fd >= 0) close(m_impl->stdout_fd);
    if (!m_impl->reaped) {
        KillGroup();
        Wait();
    }
}

} // namespace util

#else

#include <windows.h>

namespace util {

struct ContainedChild::Impl {
    HANDLE job{nullptr};
    HANDLE process{nullptr};
    HANDLE thread{nullptr};
    HANDLE stdout_rd{nullptr};
    HANDLE nul{INVALID_HANDLE_VALUE};
    bool exited{false};
    int exit_code{-1};
};

// WHY A JOB OBJECT, now that there is no shell. The child used to be
// `cmd.exe /S /C "<program> ..."`, which made the real program a GRANDCHILD: a
// handle to the spawned process was a handle to the shell, and terminating it
// orphaned the program with the card pinned. The bridge now spawns directly, so
// that particular grandchild is gone, but the job object stays and is not
// belt-and-braces: an operator-supplied path is routinely a wrapper, a wrapper
// need not exec, and a CUDA process can spawn helpers of its own. A process
// handle kills one process; the job object kills the unit. It is the faithful
// analogue of POSIX setpgid + killpg. KILL_ON_JOB_CLOSE additionally guarantees
// no straggler survives an early return on any error path -- including this
// process dying without running its shutdown.
std::unique_ptr<ContainedChild> ContainedChild::Spawn(const std::vector<std::string>& argv,
                                                      ChildStdout stdout_mode,
                                                      SpawnFault inject)
{
    if (argv.empty()) return nullptr;
    const std::string cmdline = WindowsCommandLine(argv);
    const bool want_pipe = (stdout_mode == ChildStdout::kPipe);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (inject == SpawnFault::kPipe) return nullptr;
    if (want_pipe && !CreatePipe(&rd, &wr, &sa, 0)) return nullptr;
    // The read end is ours alone. Left inheritable, the child would hold a copy
    // and the pipe would never report EOF even after the child exited.
    if (want_pipe) SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    // STARTF_USESTDHANDLES requires all three to be valid. Under a scheduled
    // task, and under quicksilver, there is no console, so GetStdHandle can
    // hand back NULL -- open the NUL device instead of trusting it.
    HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);

    // job_ok, not `job != nullptr`: a handle we hold but never configured or
    // never assigned the process to kills NOTHING when we terminate it, and the
    // caller would go on believing it had reaped somebody. Every step has to
    // succeed before the handle means containment.
    HANDLE job = (inject == SpawnFault::kContainment) ? nullptr : CreateJobObjectW(nullptr, nullptr);
    bool job_ok = false;
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        job_ok = SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli)) != 0;
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = want_pipe ? wr : nul;
    si.hStdError = (nul != INVALID_HANDLE_VALUE) ? nul : si.hStdOutput;
    si.hStdInput = (nul != INVALID_HANDLE_VALUE) ? nul : nullptr;

    // CreateProcess writes to this buffer, so it cannot be the string's storage.
    std::vector<char> cmdbuf(cmdline.begin(), cmdline.end());
    cmdbuf.push_back('\0');

    PROCESS_INFORMATION pi{};
    // lpApplicationName stays null so that CreateProcess resolves the name
    // against PATH and appends .exe, as the shell used to. The command line's
    // first token is quoted by WindowsCommandLine, which is what keeps that
    // resolution from guessing where a path with a space ends.
    //
    // CREATE_SUSPENDED so the process is in the job BEFORE it can run and spawn
    // anything of its own; otherwise a fast child escapes the job and survives
    // the kill. CREATE_NO_WINDOW keeps a console from flashing over the GUI.
    const BOOL spawned = (inject == SpawnFault::kSpawn) ? FALSE
                       : CreateProcessA(nullptr, cmdbuf.data(), nullptr, nullptr, TRUE,
                                        CREATE_SUSPENDED | CREATE_NO_WINDOW,
                                        nullptr, nullptr, &si, &pi);
    if (want_pipe) CloseHandle(wr);  // parent must drop its write end or the pipe never sees EOF
    if (!spawned) {
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (job) CloseHandle(job);
        if (rd) CloseHandle(rd);
        return nullptr;
    }
    if (job_ok) job_ok = AssignProcessToJobObject(job, pi.hProcess) != 0;
    // FAIL CLOSED. Without the job object the only kill available is against
    // this one process handle, which reaches the child but nothing the child
    // started. Degrading to that weaker kill would let a cancel report a clean
    // stop over a process still running with the card held.
    //
    // This is the one moment where refusing costs nothing: CREATE_SUSPENDED
    // means the process has not executed a single instruction, so there is
    // nothing to orphan yet.
    if (!job_ok) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (job) CloseHandle(job);
        if (rd) CloseHandle(rd);
        return nullptr;
    }
    ResumeThread(pi.hThread);

    auto impl = std::make_unique<Impl>();
    impl->job = job;
    impl->process = pi.hProcess;
    impl->thread = pi.hThread;
    impl->stdout_rd = rd;
    impl->nul = nul;
    return std::unique_ptr<ContainedChild>(new ContainedChild(std::move(impl)));
}

ContainedChild::ReadStatus ContainedChild::ReadStdout(char* buf, size_t buflen, int timeout_ms, size_t& got)
{
    got = 0;
    if (!m_impl->stdout_rd) return ReadStatus::kClosed;
    // Poll finely and account coarsely: the caller's timeout is its silence
    // budget, and a 10 ms poll costs nothing while keeping the answer prompt.
    constexpr int kPollSliceMs = 10;
    int waited = 0;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(m_impl->stdout_rd, nullptr, 0, nullptr, &avail, nullptr)) return ReadStatus::kClosed;
        if (avail > 0) {
            const DWORD want = avail > buflen ? static_cast<DWORD>(buflen) : avail;
            DWORD n = 0;
            if (!ReadFile(m_impl->stdout_rd, buf, want, &n, nullptr) || n == 0) return ReadStatus::kClosed;
            got = n;
            return ReadStatus::kData;
        }
        // Nothing buffered. If the process is also gone, that is a true EOF;
        // anything it wrote is already in the pipe and would show as avail > 0.
        if (WaitForSingleObject(m_impl->process, 0) == WAIT_OBJECT_0) return ReadStatus::kClosed;
        if (waited >= timeout_ms) return ReadStatus::kTimeout;
        const int slice = std::min(kPollSliceMs, timeout_ms - waited);
        Sleep(static_cast<DWORD>(slice));
        waited += slice;
    }
}

bool ContainedChild::Exited()
{
    if (m_impl->exited) return true;
    if (WaitForSingleObject(m_impl->process, 0) != WAIT_OBJECT_0) return false;
    DWORD code = 1;
    GetExitCodeProcess(m_impl->process, &code);
    m_impl->exited = true;
    m_impl->exit_code = static_cast<int>(code);
    return true;
}

void ContainedChild::KillGroup()
{
    if (m_impl->exited) return;
    if (m_impl->job) TerminateJobObject(m_impl->job, 1);
}

int ContainedChild::Wait()
{
    if (m_impl->exited) return m_impl->exit_code;
    WaitForSingleObject(m_impl->process, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(m_impl->process, &code);
    m_impl->exited = true;
    m_impl->exit_code = static_cast<int>(code);
    return m_impl->exit_code;
}

ContainedChild::~ContainedChild()
{
    if (!m_impl->exited) {
        KillGroup();
        Wait();
    }
    if (m_impl->stdout_rd) CloseHandle(m_impl->stdout_rd);
    if (m_impl->thread) CloseHandle(m_impl->thread);
    if (m_impl->process) CloseHandle(m_impl->process);
    if (m_impl->nul != INVALID_HANDLE_VALUE) CloseHandle(m_impl->nul);
    if (m_impl->job) CloseHandle(m_impl->job);  // KILL_ON_JOB_CLOSE reaps anything still standing
}

} // namespace util

#endif

// Defined here, below both platform sections, and not up in the platform-neutral half:
// the parameter is a unique_ptr<Impl> by value, so its destructor is instantiated at the
// point of definition, and that needs Impl to be complete. GCC and MSVC happen to defer
// that instantiation to the end of the translation unit and so accept the definition
// where Impl is still only forward-declared; clang instantiates it eagerly and rejects it
// (`invalid application of sizeof to an incomplete type`). Keep this after the #endif.
namespace util {

ContainedChild::ContainedChild(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

} // namespace util
