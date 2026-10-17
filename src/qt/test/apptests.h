// Copyright (c) 2018-2020 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_TEST_APPTESTS_H
#define QUICKSILVER_QT_TEST_APPTESTS_H

#include <QObject>
#include <set>
#include <string>
#include <utility>

class QuicksilverApplication;
class QuicksilverGUI;
class RPCConsole;

class AppTests : public QObject
{
    Q_OBJECT
public:
    explicit AppTests(QuicksilverApplication& app) : m_app(app) {}

private Q_SLOTS:
    void restartArgumentsForDeveloperNetwork();
    void consensusTorPreflight();
    // These four run before appTests. appTests starts the shared node and then
    // shuts it down; a slot after that cannot open Options with no client model
    // or prove a restart before the node thread exists. Sibling slices append
    // at the end of this list, so this block stays clear of that region.
    void optionsOpensWithConsensusOff();
    void optionsSaveWithNoNode();
    void networkPageOpensWithConsensusOff();
    void headerHidesTheCurrentPagesButton();
    void appTests();
    void guiTests(QuicksilverGUI* window);
    void consoleTests(RPCConsole* console);

private:
    //! Driven from guiTests, where a real window and node already exist.
    void shutdownIsNotParkedBehindAModalDialog(QuicksilverGUI* window);

    //! The consensus-off window the four slots above share. Not created through
    //! baseInitialize, so it does not leave a vault controller for appTests.
    QuicksilverGUI* consensusOffWindow();
    //! Hide that window and drop the options model it borrowed, before appTests
    //! creates its own. Does not delete the window: createWindow owns that pointer.
    void releaseConsensusOffWindow();
    //! The one setupPlatformStyle() call for this process. Each call replaces
    //! the app's style without freeing the old one, and the consensus-off
    //! window still holds the first, so a second call would leak it.
    void setupPlatformStyleOnce();

    //! Add expected callback name to list of pending callbacks.
    void expectCallback(std::string callback) { m_callbacks.emplace(std::move(callback)); }

    //! RAII helper to remove no-longer-pending callback.
    struct HandleCallback
    {
        std::string m_callback;
        AppTests& m_app_tests;
        ~HandleCallback();
    };

    //! Quicksilver application.
    QuicksilverApplication& m_app;

    //! Set of pending callback names. Used to track expected callbacks and shut
    //! down the app after the last callback has been handled and all tests have
    //! either run or thrown exceptions. This could be a simple int counter
    //! instead of a set of names, but the names might be useful for debugging.
    std::multiset<std::string> m_callbacks;

    //! Set by setupPlatformStyleOnce.
    bool m_platform_style_set{false};
};

#endif // QUICKSILVER_QT_TEST_APPTESTS_H
