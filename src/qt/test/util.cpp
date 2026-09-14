// Copyright (c) 2018-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/util.h>

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QString>
#include <QTest>
#include <QTimer>
#include <QWidget>

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
