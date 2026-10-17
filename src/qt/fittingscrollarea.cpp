// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/fittingscrollarea.h>

#include <QEvent>
#include <QScrollBar>

FittingScrollArea::FittingScrollArea(QWidget* parent)
    : QScrollArea(parent)
{
    // The horizontal bar comes and goes with the width; the hint counts it.
    horizontalScrollBar()->installEventFilter(this);
}

QSize FittingScrollArea::sizeHint() const
{
    const QSize stock = QScrollArea::sizeHint();
    if (!widget() || !widgetResizable()) return stock;
    int height = widget()->sizeHint().height() + 2 * frameWidth();
    if (horizontalScrollBar()->isVisible()) height += horizontalScrollBar()->sizeHint().height();
    return QSize(stock.width(), qMin(height, MAX_LINES * fontMetrics().height()));
}

bool FittingScrollArea::eventFilter(QObject* watched, QEvent* event)
{
    // QScrollArea already filters the contents' events.
    const bool contents_changed = watched == widget() && event->type() == QEvent::LayoutRequest;
    const bool bar_changed = watched == horizontalScrollBar() && (event->type() == QEvent::Show || event->type() == QEvent::Hide);
    if (contents_changed || bar_changed) updateGeometry();
    return QScrollArea::eventFilter(watched, event);
}
