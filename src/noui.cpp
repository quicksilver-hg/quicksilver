// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <noui.h>

#include <logging.h>
#include <node/interface_ui.h>
#include <util/translation.h>

#include <string>

#include <boost/signals2/connection.hpp>
#include <boost/signals2/signal.hpp>

/** Store connections so we can disconnect them when suppressing output */
boost::signals2::connection noui_ThreadSafeMessageBoxConn;
boost::signals2::connection noui_ThreadSafeQuestionConn;
boost::signals2::connection noui_InitMessageConn;

//! A style without its buttons and modality: what kind of message it is.
static constexpr unsigned int MessageKind(unsigned int style)
{
    return style & ~(CClientUIInterface::BTN_MASK | CClientUIInterface::MODAL);
}

bool noui_ThreadSafeMessageBox(const bilingual_str& message, const std::string& caption, unsigned int style)
{
    bool fSecure = style & CClientUIInterface::SECURE;
    style &= ~CClientUIInterface::SECURE;

    // Classify by kind, not by the whole style word: a question such as the
    // chainstate-recovery prompt is MSG_ERROR | BTN_ABORT, and its added button must
    // not demote it to an uncaptioned info line.
    std::string strCaption;
    switch (MessageKind(style)) {
    case MessageKind(CClientUIInterface::MSG_ERROR):
        strCaption = "Error: ";
        if (!fSecure) LogError(HgLog::INIT, "%s\n", message.original);
        break;
    case MessageKind(CClientUIInterface::MSG_WARNING):
        strCaption = "Warning: ";
        if (!fSecure) LogWarning(HgLog::INIT, "%s\n", message.original);
        break;
    case MessageKind(CClientUIInterface::MSG_INFORMATION):
        strCaption = "Information: ";
        if (!fSecure) LogInfo(HgLog::INIT, "%s\n", message.original);
        break;
    default:
        strCaption = caption + ": "; // Use supplied caption (can be empty)
        if (!fSecure) LogInfo(HgLog::INIT, "%s%s\n", strCaption, message.original);
    }

    tfm::format(std::cerr, "%s%s\n", strCaption, message.original);
    return false;
}

bool noui_ThreadSafeQuestion(const bilingual_str& /* ignored interactive message */, const std::string& message, const std::string& caption, unsigned int style)
{
    return noui_ThreadSafeMessageBox(Untranslated(message), caption, style);
}

void noui_InitMessage(const std::string& message)
{
    LogInfo(HgLog::INIT, "init message: %s\n", message);
}

void noui_connect()
{
    noui_ThreadSafeMessageBoxConn = uiInterface.ThreadSafeMessageBox_connect(noui_ThreadSafeMessageBox);
    noui_ThreadSafeQuestionConn = uiInterface.ThreadSafeQuestion_connect(noui_ThreadSafeQuestion);
    noui_InitMessageConn = uiInterface.InitMessage_connect(noui_InitMessage);
}

bool noui_ThreadSafeMessageBoxRedirect(const bilingual_str& message, const std::string& caption, unsigned int style)
{
    LogInfo(HgLog::INIT, "%s: %s\n", caption, message.original);
    return false;
}

bool noui_ThreadSafeQuestionRedirect(const bilingual_str& /* ignored interactive message */, const std::string& message, const std::string& caption, unsigned int style)
{
    LogInfo(HgLog::INIT, "%s: %s\n", caption, message);
    return false;
}

void noui_InitMessageRedirect(const std::string& message)
{
    LogInfo(HgLog::INIT, "init message: %s\n", message);
}

void noui_test_redirect()
{
    noui_ThreadSafeMessageBoxConn.disconnect();
    noui_ThreadSafeQuestionConn.disconnect();
    noui_InitMessageConn.disconnect();
    noui_ThreadSafeMessageBoxConn = uiInterface.ThreadSafeMessageBox_connect(noui_ThreadSafeMessageBoxRedirect);
    noui_ThreadSafeQuestionConn = uiInterface.ThreadSafeQuestion_connect(noui_ThreadSafeQuestionRedirect);
    noui_InitMessageConn = uiInterface.InitMessage_connect(noui_InitMessageRedirect);
}

void noui_reconnect()
{
    noui_ThreadSafeMessageBoxConn.disconnect();
    noui_ThreadSafeQuestionConn.disconnect();
    noui_InitMessageConn.disconnect();
    noui_connect();
}
