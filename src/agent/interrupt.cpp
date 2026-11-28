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
// The handler's store is async-signal-safe only because this atomic needs no lock.
static_assert(std::atomic<bool>::is_always_lock_free, "the interrupt flag must be lock-free to be set from a signal handler");

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
// SIG_DFL is a null pointer on the Windows CRT, so null cannot mean "nothing to
// restore"; SIG_ERR marks a signal() call that failed and left nothing to undo.
void (*g_prev_int)(int) = SIG_ERR;
void (*g_prev_term)(int) = SIG_ERR;
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
    // No SA_RESTART. The solver bridge does not depend on EINTR -- it polls the
    // cancel callback every 250 ms -- but an interrupted read returning early
    // only brings that poll forward.
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
    if (g_prev_int != SIG_ERR) std::signal(SIGINT, g_prev_int);
    if (g_prev_term != SIG_ERR) std::signal(SIGTERM, g_prev_term);
    g_prev_int = SIG_ERR;
    g_prev_term = SIG_ERR;
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
