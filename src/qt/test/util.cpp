// Copyright (c) 2018-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/util.h>

#include <interfaces/node.h>
#include <qt/quicksilverstyle.h>
#include <test/util/gpucardtables.h>

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QMessageBox>
#include <QProxyStyle>
#include <QPushButton>
#include <QStyle>
#include <QStyleFactory>
#include <QString>
#include <QTest>
#include <QTimer>
#include <QWidget>

QString SolverProbeStubPath()
{
    QString path{QCoreApplication::applicationDirPath() + QStringLiteral("/test_solver_probe_stub")};
#ifdef Q_OS_WIN
    path += QStringLiteral(".exe");
#endif
    return path;
}

void ResetGpuCardTables()
{
    GpuCardTablesTestSetup::Reset();
}

const std::vector<const char*>& ProbeEnvironmentNames()
{
    static const std::vector<const char*> names{
        "CUCKATOO_GPU_SOLVER", "CUCKATOO_GPU_DEVICES", "QS_PROBE_STUB_LISTING", "QS_PROBE_STUB_EXIT", "QS_PROBE_STUB_SLEEP_MS"};
    return names;
}

ScopedEnvironment::ScopedEnvironment(const std::vector<const char*>& names)
{
    for (const char* name : names) {
        m_saved.emplace_back(name, qEnvironmentVariableIsSet(name) ? std::optional<QByteArray>{qgetenv(name)} : std::nullopt);
    }
}

ScopedEnvironment::~ScopedEnvironment()
{
    for (const auto& [name, value] : m_saved) {
        if (value) {
            qputenv(name, *value);
        } else {
            qunsetenv(name);
        }
    }
}

ScopedNodeContext::ScopedNodeContext(interfaces::Node& node, node::NodeContext& context)
    : m_node(node), m_previous_context(node.context())
{
    m_node.setContext(&context);
}

ScopedNodeContext::~ScopedNodeContext()
{
    m_node.setContext(m_previous_context);
}

RestoreApplicationFont::RestoreApplicationFont() : m_font{QApplication::font()} {}

RestoreApplicationFont::~RestoreApplicationFont()
{
    QApplication::setFont(m_font);
}

namespace {

QWidget* FindVisibleWidgetOfClass(const char* class_name)
{
    auto matches = [class_name](QWidget* widget) {
        return widget && widget->isVisible() && widget->inherits(class_name);
    };
    if (QWidget* modal = QApplication::activeModalWidget()) {
        if (matches(modal)) return modal;
        for (QObject* child : modal->findChildren<QWidget*>()) {
            if (matches(qobject_cast<QWidget*>(child))) return qobject_cast<QWidget*>(child);
        }
    }
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (matches(widget)) return widget;
        for (QObject* child : widget->findChildren<QWidget*>()) {
            if (matches(qobject_cast<QWidget*>(child))) return qobject_cast<QWidget*>(child);
        }
    }
    return nullptr;
}

void DismissModal(QWidget* modal)
{
    if (auto* box = qobject_cast<QMessageBox*>(modal)) {
        QAbstractButton* button = box->escapeButton() ? box->escapeButton() : box->defaultButton();
        if (button) {
            button->click();
            return;
        }
        box->reject();
        return;
    }
    if (auto* dlg = qobject_cast<QDialog*>(modal)) {
        dlg->reject();
        return;
    }
    if (modal) modal->close();
}

} // namespace

void ExpectModalWithoutNestedEventLoop(const char* class_name, const std::function<void()>& show_dialog)
{
    bool caller_returned = false;
    bool saw_modal = false;
    bool nested_loop = false;
    QString got_class = QStringLiteral("none");
    QTimer::singleShot(0, [&]() {
        nested_loop = !caller_returned;
        QWidget* modal = FindVisibleWidgetOfClass(class_name);
        if (QWidget* active = QApplication::activeModalWidget()) {
            got_class = QString::fromUtf8(active->metaObject()->className());
        }
        saw_modal = modal != nullptr;
        if (modal) DismissModal(modal);
    });
    show_dialog();
    caller_returned = true;
    if (!saw_modal) {
        QTRY_VERIFY_WITH_TIMEOUT(saw_modal, 1000);
    }
    QVERIFY2(!nested_loop, "dialog used QDialog::exec() (nested event loop)");
    QVERIFY2(saw_modal, qPrintable(QStringLiteral("expected visible modal %1, got %2")
                                       .arg(QString::fromUtf8(class_name), got_class)));
}

namespace {
//! The style the application installed. With a stylesheet set, qApp->style()
//! is the sheet's unnamed wrapper, which owns the installed style; recreating
//! the wrapper's empty name gives no style at all.
const QStyle* InstalledStyle()
{
    const QStyle* style{qApp->style()};
    if (!style->inherits("QStyleSheetStyle")) return style;
    if (const QProxyStyle* proxy = style->findChild<QProxyStyle*>()) return proxy;
    for (const QStyle* child : style->findChildren<QStyle*>()) {
        if (!child->inherits("QStyleSheetStyle")) return child;
    }
    return style;
}
} // namespace

RestoreApplicationStyle::RestoreApplicationStyle()
    : m_style{InstalledStyle()->objectName()},
      m_proxy{qobject_cast<const QProxyStyle*>(InstalledStyle()) != nullptr},
      m_palette{qApp->palette()},
      m_stylesheet{qApp->styleSheet()},
      m_base_style{qApp->property("quicksilverBaseStyle")}
{
}

RestoreApplicationStyle::~RestoreApplicationStyle()
{
    qApp->setStyleSheet(QString());
    QStyle* base{QStyleFactory::create(m_style)};
    // A null style would leave whatever the test installed (S0297-C-01).
    if (!base) {
        // A destructor cannot return from the slot; record the failure and
        // fall back to the style the application ships.
        QTest::qFail(qPrintable(QStringLiteral("RestoreApplicationStyle cannot recreate style \"%1\"").arg(m_style)), __FILE__, __LINE__);
        base = QStyleFactory::create(QStringLiteral("Fusion"));
    }
    // The proxy keeps its base's name; recreating by that name alone would
    // drop the proxy's style hints for every later test.
    qApp->setStyle(m_proxy ? QuicksilverStyle::WithoutDialogButtonIcons(base) : base);
    qApp->setPalette(m_palette);
    qApp->setStyleSheet(m_stylesheet);
    qApp->setProperty("quicksilverBaseStyle", m_base_style);
}
