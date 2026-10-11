// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/benchpanel.h>

#include <cassert>

#include <QBoxLayout>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSizePolicy>
#include <QVariant>
#include <QVBoxLayout>

namespace BenchPanel {

Parts Install(QFrame* frame, const QString& title)
{
    frame->setProperty("benchPanel", QStringLiteral("true"));

    auto* outer = new QVBoxLayout(frame);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* head = new QFrame(frame);
    head->setObjectName(QStringLiteral("benchPanelHead"));
    head->setFixedHeight(34);
    auto* head_layout = new QHBoxLayout(head);
    head_layout->setContentsMargins(12, 0, 10, 0);
    head_layout->setSpacing(8);
    auto* title_label = new QLabel(title.toUpper(), head);
    title_label->setObjectName(QStringLiteral("benchPanelTitle"));
    QFont title_font = title_label->font();
    title_font.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
    title_label->setFont(title_font);
    head_layout->addWidget(title_label, 1);
    outer->addWidget(head);

    auto* body_host = new QWidget(frame);
    auto* body = new QVBoxLayout(body_host);
    body->setContentsMargins(12, 10, 12, 12);
    body->setSpacing(8);
    outer->addWidget(body_host, 1);
    return {frame, head_layout, body};
}

Parts Make(const QString& object_name, const QString& title, QWidget* parent)
{
    auto* frame = new QFrame(parent);
    frame->setObjectName(object_name);
    frame->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    return Install(frame, title);
}

namespace {
//! The box layout under `layout` that holds `widget`, and its index there.
QBoxLayout* FindBox(QLayout* layout, QWidget* widget, int* index)
{
    if (!layout) return nullptr;
    for (int i = 0; i < layout->count(); ++i) {
        QLayoutItem* item = layout->itemAt(i);
        if (item->widget() == widget) {
            *index = i;
            return qobject_cast<QBoxLayout*>(layout);
        }
        if (QBoxLayout* found = FindBox(item->layout(), widget, index)) return found;
    }
    return nullptr;
}
} // namespace

Parts Wrap(QWidget* content, const QString& object_name, const QString& title)
{
    QWidget* parent = content->parentWidget();
    int index = -1;
    QBoxLayout* box = parent ? FindBox(parent->layout(), content, &index) : nullptr;
    assert(box);
    const int stretch = box->stretch(index);
    box->removeWidget(content);

    Parts parts = Make(object_name, title, parent);
    parts.frame->setSizePolicy(content->sizePolicy());
    content->setProperty("benchBody", true);
    parts.body->addWidget(content);
    box->insertWidget(index, parts.frame, stretch);
    return parts;
}

Parts Adopt(QWidget* dialog, QWidget* buttons, const QString& object_name, const QString& title)
{
    auto* top = qobject_cast<QBoxLayout*>(dialog->layout());
    assert(top);
    if (buttons) {
        int index = -1;
        QBoxLayout* holder = FindBox(top, buttons, &index);
        assert(holder);
        holder->removeWidget(buttons);
    }

    // Everything the form laid out moves into the panel body, in the form's own
    // direction, so a side-by-side form stays side by side.
    Parts parts = Make(object_name, title, dialog);
    // A dialog's panel takes the dialog's free height, as the Options panel does.
    parts.frame->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto* content = new QBoxLayout(top->direction());
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(top->spacing());
    while (top->count() > 0) {
        const int stretch = top->stretch(0);
        QLayoutItem* item = top->takeAt(0);
        if (QWidget* widget = item->widget()) {
            content->addWidget(widget, stretch);
            delete item;
        } else if (QLayout* layout = item->layout()) {
            layout->setParent(nullptr);
            content->addLayout(layout, stretch);
        } else {
            content->addItem(item);
        }
    }
    parts.body->addLayout(content, 1);

    top->setDirection(QBoxLayout::TopToBottom);
    top->setContentsMargins(18, 16, 18, 18);
    top->setSpacing(12);
    top->addWidget(parts.frame, 1);
    if (buttons) top->addWidget(buttons);
    return parts;
}

QGridLayout* MakeRows(QWidget* host)
{
    auto* grid = new QGridLayout(host);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(7);
    grid->setColumnStretch(1, 1);
    return grid;
}

QLabel* AddRow(QGridLayout* rows, const QString& key, const QString& value_name, QWidget* parent, QLabel** key_out)
{
    const int row = rows->rowCount();
    auto* key_label = new QLabel(key, parent);
    key_label->setProperty("class", QStringLiteral("benchKey"));
    auto* value = new QLabel(parent);
    value->setObjectName(value_name);
    value->setProperty("class", QStringLiteral("benchValue"));
    value->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rows->addWidget(key_label, row, 0);
    rows->addWidget(value, row, 1);
    if (key_out) *key_out = key_label;
    return value;
}

} // namespace BenchPanel
