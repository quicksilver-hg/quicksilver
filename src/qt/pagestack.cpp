// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/pagestack.h>

#include <QMargins>
#include <QSize>

QSize PageStack::sizeHint() const
{
    const QWidget* page{currentWidget()};
    if (!page) return QStackedWidget::sizeHint();
    return page->sizeHint().grownBy(contentsMargins());
}

QSize PageStack::minimumSizeHint() const
{
    const QWidget* page{currentWidget()};
    if (!page) return QStackedWidget::minimumSizeHint();
    return page->minimumSizeHint().grownBy(contentsMargins());
}

bool PageStack::hasHeightForWidth() const
{
    const QWidget* page{currentWidget()};
    if (!page) return QStackedWidget::hasHeightForWidth();
    return page->hasHeightForWidth();
}

int PageStack::heightForWidth(int width) const
{
    const QWidget* page{currentWidget()};
    if (!page || !page->hasHeightForWidth()) return -1;
    const QMargins margins{contentsMargins()};
    return page->heightForWidth(width - margins.left() - margins.right()) + margins.top() + margins.bottom();
}
