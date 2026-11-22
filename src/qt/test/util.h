// Copyright (c) 2018-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_TEST_UTIL_H
#define QUICKSILVER_QT_TEST_UTIL_H

#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QFileInfo>
#include <QFont>
#include <QPalette>
#include <QString>
#include <QVariant>

namespace interfaces {
class Node;
} // namespace interfaces
namespace node {
struct NodeContext;
} // namespace node

/**
 * Run `show_dialog` and require that it returns while a modal widget of
 * `class_name` is still the active modal. Fails if `show_dialog` used
 * QDialog::exec() (nested event loop). Closes the modal afterwards.
 */
void ExpectModalWithoutNestedEventLoop(const char* class_name, const std::function<void()>& show_dialog);
QString SolverProbeStubPath();
void ResetGpuCardTables();

//! Fails the calling slot, naming the path, when the probe stub is not beside
//! the test binary (an install copies only test_quicksilver-qt).
#define QVERIFY_SOLVER_PROBE_STUB()                                                              \
    QVERIFY2(QFileInfo::exists(SolverProbeStubPath()),                                           \
             qPrintable(QStringLiteral("test_solver_probe_stub not found at ") + SolverProbeStubPath()))

//! The variables the GPU discovery tests set to steer the probe stub.
const std::vector<const char*>& ProbeEnvironmentNames();

/**
 * Snapshots the named environment variables and puts each back (set to its
 * old value, or unset) on scope exit, even when an assertion returns early.
 */
class ScopedEnvironment
{
public:
    explicit ScopedEnvironment(const std::vector<const char*>& names);
    ~ScopedEnvironment();
    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

private:
    std::vector<std::pair<const char*, std::optional<QByteArray>>> m_saved;
};

/**
 * Points `node` at `context` and puts the previous context back on scope exit.
 * Declare it after the setup that owns `context`, so the node lets go of the
 * context before the setup is destroyed.
 */
class ScopedNodeContext
{
public:
    ScopedNodeContext(interfaces::Node& node, node::NodeContext& context);
    ~ScopedNodeContext();
    ScopedNodeContext(const ScopedNodeContext&) = delete;
    ScopedNodeContext& operator=(const ScopedNodeContext&) = delete;

private:
    interfaces::Node& m_node;
    node::NodeContext* const m_previous_context;
};

//! Restores the application font on scope exit.
class RestoreApplicationFont
{
public:
    RestoreApplicationFont();
    ~RestoreApplicationFont();
    RestoreApplicationFont(const RestoreApplicationFont&) = delete;
    RestoreApplicationFont& operator=(const RestoreApplicationFont&) = delete;

private:
    QFont m_font;
};

/**
 * QuicksilverStyle::Apply replaces the base style as well as the palette and
 * stylesheet. Restores all three on scope exit, even when an assertion
 * returns early. A saved Quicksilver proxy style is reinstalled as the proxy,
 * not as the plain base style its name would recreate.
 */
class RestoreApplicationStyle
{
public:
    RestoreApplicationStyle();
    ~RestoreApplicationStyle();
    RestoreApplicationStyle(const RestoreApplicationStyle&) = delete;
    RestoreApplicationStyle& operator=(const RestoreApplicationStyle&) = delete;

private:
    QString m_style;
    bool m_proxy;
    QPalette m_palette;
    QString m_stylesheet;
    QVariant m_base_style;
};

#endif // QUICKSILVER_QT_TEST_UTIL_H
