// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_PAGESTACK_H
#define QUICKSILVER_QT_PAGESTACK_H

#include <QStackedWidget>

/**
 * A stack of pages that asks for only as much room as the page on screen needs.
 *
 * QStackedLayout answers every sizing question with the largest of *all* its
 * pages, hidden ones included: sizeHint(), minimumSize(), and heightForWidth(),
 * the last of which says in Qt's own source that the size policy is
 * deliberately not consulted -- so the usual trick of marking hidden pages
 * QSizePolicy::Ignored cannot reach it.
 *
 * One tall page therefore makes every other page demand a height it has no
 * content for. Inside a scroll area that resizes its widget, which is how the
 * main window presents these, the demand is granted, and any page that pins a
 * control below its content carries that control past the bottom of the
 * viewport. The network page needs about 1250 px; the transfer page has 158 px
 * of content and a Transmit button under it. On a full 1080p screen that button
 * sat 300 px below the bottom of the window, so a sender had to scroll away
 * from the address they had typed to reach the irreversible action.
 *
 * Each question is answered from the current page instead. Pages that are
 * genuinely taller than the window still scroll -- that is what the main
 * window's scroll area is for -- but only when they are the page being shown.
 */
class PageStack : public QStackedWidget
{
    Q_OBJECT

public:
    explicit PageStack(QWidget* parent = nullptr) : QStackedWidget(parent) {}

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
};

#endif // QUICKSILVER_QT_PAGESTACK_H
