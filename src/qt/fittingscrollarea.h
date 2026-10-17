// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_FITTINGSCROLLAREA_H
#define QUICKSILVER_QT_FITTINGSCROLLAREA_H

#include <QScrollArea>

/** A resizable scroll area whose size hint follows its contents.
 *
 * QScrollArea keeps the contents' size hint from when the widget was set, so
 * an area capped at its own hint clips contents that grow later, and it does
 * not count an as-needed horizontal bar. This one asks the contents each time,
 * adds the horizontal bar while it is shown, and keeps QScrollArea's cap of
 * 24 lines, past which the contents scroll.
 */
class FittingScrollArea : public QScrollArea
{
    Q_OBJECT

public:
    explicit FittingScrollArea(QWidget* parent = nullptr);

    QSize sizeHint() const override;
    bool eventFilter(QObject* watched, QEvent* event) override;

    //! The tallest hint, in lines of the area's font, as QScrollArea's own.
    static constexpr int MAX_LINES{24};
};

#endif // QUICKSILVER_QT_FITTINGSCROLLAREA_H
