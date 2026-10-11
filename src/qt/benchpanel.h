// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_BENCHPANEL_H
#define QUICKSILVER_QT_BENCHPANEL_H

#include <QString>

QT_BEGIN_NAMESPACE
class QFrame;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QVBoxLayout;
class QWidget;
QT_END_NAMESPACE

//! The desktop's page grammar: content sits in panels with a small-caps title
//! row, and state reads as label/value rows.
namespace BenchPanel {

struct Parts {
    QFrame* frame{nullptr};
    QHBoxLayout* head{nullptr};
    QVBoxLayout* body{nullptr};
};

//! Lays `frame` out as a panel: a title row (the title in capitals, room at
//! its right for a trailing widget) over a body. The frame must not have a
//! layout yet.
Parts Install(QFrame* frame, const QString& title);

//! A new panel frame named `object_name`.
Parts Make(const QString& object_name, const QString& title, QWidget* parent);

//! Puts `content`, which a form already laid out in a box layout, inside a
//! new panel named `object_name` that takes its place. The content is marked
//! as a panel body (property benchBody), so the sheet draws it flat.
Parts Wrap(QWidget* content, const QString& object_name, const QString& title);

//! Moves everything a dialog's form laid out into a new titled panel named
//! `object_name`, keeping `buttons` (the dialog's own button row, or null) below
//! it, outside the panel. The dialog's top layout must be a box layout.
Parts Adopt(QWidget* dialog, QWidget* buttons, const QString& object_name, const QString& title);

//! A grid for label/value rows, laid on `host`.
QGridLayout* MakeRows(QWidget* host);

//! Appends a row: the key muted at the left, the value right-aligned in the
//! fixed-pitch face. Returns the value label, named `value_name`.
QLabel* AddRow(QGridLayout* rows, const QString& key, const QString& value_name, QWidget* parent, QLabel** key_out = nullptr);

} // namespace BenchPanel

#endif // QUICKSILVER_QT_BENCHPANEL_H
