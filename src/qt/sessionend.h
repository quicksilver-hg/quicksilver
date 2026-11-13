// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_SESSIONEND_H
#define QUICKSILVER_QT_SESSIONEND_H

#include <atomic>
#include <chrono>
#include <functional>

//! F-591: what the GUI does when the desktop session ends underneath it.
//!
//! With a system Qt on X11, QXcbSessionManager connects to the X session
//! manager over ICE (when SESSION_MANAGER is set), and Qt installs no ICE
//! IO-error handler. When the session manager goes away without a clean
//! `die`, libICE's default handler calls exit(1) on the Qt main thread, which
//! skips Shutdown() and runs static destructors with the node live.
namespace SessionEnd {

using QuitAction = std::function<void()>;

//! The session manager connection was lost. Asks for an orderly quit, the same
//! path File > Quit takes, exactly once: \p already_requested is the caller's
//! once-flag, so a second report cannot start a second shutdown over the first.
void OnSessionManagerLost(const QuitAction& quit, bool& already_requested);

//! Replaces libICE's default IO-error handler with one that calls
//! OnSessionManagerLost(quit). Returns false, and does nothing, in a build
//! with no libICE (Windows, macOS, static-Qt depends builds).
//! Must be called before the event loop runs.
bool InstallIceIoErrorHandler(QuitAction quit);

//! F-604: lets a process that is exiting wait for a node shutdown already
//! running on another thread.
class NodeShutdownLatch
{
public:
    void MarkStarted() { m_started = true; }
    void MarkFinished() { m_finished = true; }
    //! Returns once no shutdown is outstanding, true; or at \p limit, false.
    bool WaitUntilClear(std::chrono::milliseconds limit) const;

private:
    std::atomic<bool> m_started{false};
    std::atomic<bool> m_finished{false};
};

//! The application's latch. Never destroyed, so it outlives static destructors.
NodeShutdownLatch& ExitLatch();

//! When the X server goes away Qt calls exit(1) on the main thread, and at a
//! logout that can be while the node is shutting down. Registers an atexit
//! handler that lets that shutdown finish, bounded. Call it when the shutdown
//! starts and no earlier: see the note at its call site.
void InstallExitWait();

} // namespace SessionEnd

#endif // QUICKSILVER_QT_SESSIONEND_H
