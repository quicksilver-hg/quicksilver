// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_DEVELOPEROPTIONSTOGGLE_H
#define QUICKSILVER_QT_DEVELOPEROPTIONSTOGGLE_H

#include <QPushButton>

/** The expand/collapse control for a page's developer options, shared by the
 * Mine and Network pages. It reads "+ Show developer options" collapsed and
 * "− Hide developer options" expanded, starts collapsed, and is not kept between
 * launches. A page locks it open while something it hides is in use.
 */
class DeveloperOptionsToggle : public QPushButton
{
    Q_OBJECT

public:
    explicit DeveloperOptionsToggle(QWidget* parent = nullptr);

    //! Locked open, with `reason` as its tooltip, while `reason` is non-empty;
    //! unlocked (and left as it is) otherwise.
    void setForcedOpen(const QString& reason);

private:
    void updateText();
};

#endif // QUICKSILVER_QT_DEVELOPEROPTIONSTOGGLE_H
