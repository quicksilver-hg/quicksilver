// Copyright (c) 2018-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <compat/stdin.h>

#include <cstdio>
#include <sys/stat.h>

#ifdef WIN32
#include <conio.h>
#include <io.h>
#include <windows.h>
#else
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

struct NoechoInst::RestoreState {
#ifdef WIN32
    HANDLE handle;
    DWORD mode;
    RestoreState(HANDLE h, DWORD m) : handle(h), mode(m) {}
    ~RestoreState() { SetConsoleMode(handle, mode); }
#else
    struct termios mode;
    explicit RestoreState(const struct termios& m) : mode(m) {}
    ~RestoreState() { (void)tcsetattr(STDIN_FILENO, TCSANOW, &mode); }
#endif
};

bool StdinTerminal()
{
#ifdef WIN32
    return _isatty(_fileno(stdin));
#else
    return isatty(fileno(stdin));
#endif
}

bool StdinHasBufferedInput()
{
#ifdef WIN32
    return _kbhit() != 0;
#else
    struct pollfd fds;
    fds.fd = STDIN_FILENO;
    fds.events = POLLIN;
    return poll(&fds, 1, 0) == 1 && (fds.revents & POLLIN) != 0;
#endif
}

bool StdinIsRegularFile()
{
#ifdef WIN32
    struct _stat status;
    return _fstat(_fileno(stdin), &status) == 0 && (status.st_mode & _S_IFMT) == _S_IFREG;
#else
    struct stat status;
    return fstat(fileno(stdin), &status) == 0 && S_ISREG(status.st_mode);
#endif
}

bool StdinIsMidFile()
{
    if (!StdinIsRegularFile()) return false;
#ifdef WIN32
    return _telli64(_fileno(stdin)) > 0;
#else
    return lseek(fileno(stdin), 0, SEEK_CUR) > 0;
#endif
}

NoechoInst::NoechoInst()
{
#ifdef WIN32
    HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (handle == INVALID_HANDLE_VALUE || !GetConsoleMode(handle, &mode)) return;
    auto state = std::make_unique<RestoreState>(handle, mode);
    if (!SetConsoleMode(handle, mode & ~ENABLE_ECHO_INPUT)) return;
    m_state = std::move(state);
#else
    struct termios mode;
    if (tcgetattr(STDIN_FILENO, &mode) != 0) return;
    struct termios no_echo = mode;
    no_echo.c_lflag &= ~ECHO;
    auto state = std::make_unique<RestoreState>(mode);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &no_echo) != 0) return;
    m_state = std::move(state);
#endif
}
NoechoInst::~NoechoInst() = default;
