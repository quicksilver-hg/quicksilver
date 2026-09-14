// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <agent/interrupt.h>

#include <atomic>
#include <csignal>

namespace {
//! A handler cannot capture, so the flag is file-static. InterruptHandler owns
//! its lifetime: the constructor clears it and the destructor restores the
//! previous disposition, so nothing leaks between runs or between tests.
std::atomic<bool> g_interrupted{false};

extern "C" void HandleAgentInterrupt(int)
{
    // Everything reachable from a signal handler must be async-signal-safe. A
    // relaxed store to a lock-free atomic is; formatting a message or touching
    // the grind's state would not be, which is why the handler only raises the
    // flag and the polling happens on the grind's own thread.
    g_interrupted.store(true, std::memory_order_relaxed);
}

#ifndef WIN32
struct sigaction g_prev_int{};
struct sigaction g_prev_term{};
#else
void (*g_prev_int)(int) = nullptr;
void (*g_prev_term)(int) = nullptr;
#endif
} // namespace

namespace agent {

InterruptHandler::InterruptHandler()
{
    g_interrupted.store(false, std::memory_order_relaxed);
#ifndef WIN32
    struct sigaction sa{};
    sa.sa_handler = HandleAgentInterrupt;
    sigemptyset(&sa.sa_mask);
    // Deliberately NOT SA_RESTART: a blocked read in the solver bridge must
    // return EINTR so the cancel poll gets a turn.
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, &g_prev_int);
    sigaction(SIGTERM, &sa, &g_prev_term);
#else
    g_prev_int = std::signal(SIGINT, HandleAgentInterrupt);
    g_prev_term = std::signal(SIGTERM, HandleAgentInterrupt);
#endif
}

InterruptHandler::~InterruptHandler()
{
#ifndef WIN32
    sigaction(SIGINT, &g_prev_int, nullptr);
    sigaction(SIGTERM, &g_prev_term, nullptr);
#else
    if (g_prev_int) std::signal(SIGINT, g_prev_int);
    if (g_prev_term) std::signal(SIGTERM, g_prev_term);
#endif
    g_interrupted.store(false, std::memory_order_relaxed);
}

bool InterruptHandler::Interrupted() const
{
    return g_interrupted.load(std::memory_order_relaxed);
}

cuckatoo::SolverCancelCallback InterruptHandler::Cancel() const
{
    return [] { return g_interrupted.load(std::memory_order_relaxed); };
}

} // namespace agent
