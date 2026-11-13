// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/sessionend.h>

#include <utility>

#if defined(QS_HAVE_ICE)
#include <X11/ICE/ICElib.h>
#endif

#include <cstdlib>
#include <thread>

namespace SessionEnd {

bool NodeShutdownLatch::WaitUntilClear(std::chrono::milliseconds limit) const
{
    const auto deadline{std::chrono::steady_clock::now() + limit};
    while (m_started && !m_finished) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return true;
}

NodeShutdownLatch& ExitLatch()
{
    static NodeShutdownLatch* const latch{new NodeShutdownLatch};
    return *latch;
}

void InstallExitWait()
{
    // The bound keeps a shutdown that has hung from holding the process forever.
    std::atexit([] { ExitLatch().WaitUntilClear(std::chrono::seconds{60}); });
}

void OnSessionManagerLost(const QuitAction& quit, bool& already_requested)
{
    if (already_requested) return;
    already_requested = true;
    if (quit) quit();
}

#if defined(QS_HAVE_ICE)
namespace {
// libICE's handler type carries no user data, so the action and its once-flag
// are file-local. Both are touched only on the Qt main thread, from inside
// IceProcessMessages (QSmSocketReceiver::socketActivated), and set before the
// event loop runs.
QuitAction g_quit;
bool g_quit_requested{false};

void IceIoError(IceConn)
{
    // Returning, unlike libICE's default, leaves the process alive. The
    // connection is marked dead by libICE and the action queues the orderly
    // shutdown.
    OnSessionManagerLost(g_quit, g_quit_requested);
}
} // namespace

bool InstallIceIoErrorHandler(QuitAction quit)
{
    g_quit = std::move(quit);
    g_quit_requested = false;
    IceSetIOErrorHandler(IceIoError);
    return true;
}
#else
bool InstallIceIoErrorHandler(QuitAction)
{
    return false;
}
#endif

} // namespace SessionEnd
