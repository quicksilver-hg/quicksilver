// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QUICKSILVER_AGENT_INTERRUPT_H
#define QUICKSILVER_AGENT_INTERRUPT_H

#include <crypto/cuckatoo/cuckatoo.h>

namespace agent {

/**
 * Ctrl-C handling for a command-line agent run.
 *
 * quicksilver-agent had none, and that is worse than it sounds: gpu_solver puts
 * the solver in its own process group (POSIX) or job object (Windows), so the
 * terminal's SIGINT never reaches it. Without a handler the agent dies on the
 * default action and ORPHANS qsgpusolve, which keeps the card pinned on
 * abandoned work with up to 2^24 attempts still to burn. Catching the signal and
 * cancelling the grind lets the solver bridge kill its own child on the way out.
 *
 * RAII so a test can install and drop the handler without leaving the process's
 * disposition changed, and so main() cannot forget to restore it.
 */
class InterruptHandler
{
public:
    InterruptHandler();
    ~InterruptHandler();

    InterruptHandler(const InterruptHandler&) = delete;
    InterruptHandler& operator=(const InterruptHandler&) = delete;

    //! True once SIGINT or SIGTERM has been delivered.
    bool Interrupted() const;

    //! Poll-able form for the solver bridge and the prove loop.
    cuckatoo::SolverCancelCallback Cancel() const;
};

} // namespace agent

#endif // QUICKSILVER_AGENT_INTERRUPT_H
