// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/sessionendtests.h>

#include <qt/sessionend.h>

#include <chrono>
#include <thread>

#if defined(QS_HAVE_ICE)
#include <X11/ICE/ICElib.h>
#endif

void SessionEndTests::lostSessionManagerRequestsOneQuit()
{
    int quits{0};
    bool requested{false};
    SessionEnd::OnSessionManagerLost([&quits] { ++quits; }, requested);
    QVERIFY(requested);
    QCOMPARE(quits, 1);
}

void SessionEndTests::repeatedLossStillQuitsOnce()
{
    // libICE reports an IO error per connection, and a second one must not
    // start a second shutdown over the first.
    int quits{0};
    bool requested{false};
    SessionEnd::QuitAction quit{[&quits] { ++quits; }};
    SessionEnd::OnSessionManagerLost(quit, requested);
    SessionEnd::OnSessionManagerLost(quit, requested);
    SessionEnd::OnSessionManagerLost(quit, requested);
    QCOMPARE(quits, 1);
}

void SessionEndTests::emptyQuitActionIsHarmless()
{
    bool requested{false};
    SessionEnd::OnSessionManagerLost(SessionEnd::QuitAction{}, requested);
}

void SessionEndTests::exitLatchIsClearWhenNoShutdownStarted()
{
    SessionEnd::NodeShutdownLatch latch;
    QVERIFY(latch.WaitUntilClear(std::chrono::milliseconds{0}));
    // A shutdown that reports finishing without being started holds nothing.
    latch.MarkFinished();
    QVERIFY(latch.WaitUntilClear(std::chrono::milliseconds{0}));
}

void SessionEndTests::exitLatchHoldsAnUnfinishedShutdown()
{
    SessionEnd::NodeShutdownLatch latch;
    latch.MarkStarted();
    QVERIFY(!latch.WaitUntilClear(std::chrono::milliseconds{0}));
}

void SessionEndTests::exitLatchClearsWhenTheShutdownFinishes()
{
    SessionEnd::NodeShutdownLatch latch;
    latch.MarkStarted();
    std::thread finisher{[&latch] { latch.MarkFinished(); }};
    // The bound is only a backstop: the outcome does not depend on the clock.
    const bool cleared{latch.WaitUntilClear(std::chrono::minutes{10})};
    finisher.join();
    QVERIFY(cleared);
}

#if defined(QS_HAVE_ICE)

void SessionEndTests::installedIceHandlerReplacesTheExitingDefault()
{
    // IceSetIOErrorHandler(nullptr) re-installs libICE's default and returns
    // what was installed, so this reads the handler without invoking it.
    const IceIOErrorHandler libice_default{IceSetIOErrorHandler(nullptr)};
    QVERIFY(libice_default != nullptr);

    int quits{0};
    QVERIFY(SessionEnd::InstallIceIoErrorHandler([&quits] { ++quits; }));
    const IceIOErrorHandler installed{IceSetIOErrorHandler(nullptr)};
    QVERIFY(installed != nullptr);
    QVERIFY(installed != libice_default);
}

void SessionEndTests::installedIceHandlerRequestsTheQuit()
{
    int quits{0};
    QVERIFY(SessionEnd::InstallIceIoErrorHandler([&quits] { ++quits; }));
    const IceIOErrorHandler installed{IceSetIOErrorHandler(nullptr)};
    QVERIFY(installed != nullptr);
    installed(nullptr);
    installed(nullptr);
    QCOMPARE(quits, 1);
}

#else

void SessionEndTests::installedIceHandlerReplacesTheExitingDefault()
{
    QVERIFY(!SessionEnd::InstallIceIoErrorHandler([] {}));
}

void SessionEndTests::installedIceHandlerRequestsTheQuit()
{
    QSKIP("No libICE in this build: nothing to install.");
}

#endif
