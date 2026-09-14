// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_QUICKSILVERSTYLE_H
#define QUICKSILVER_QT_QUICKSILVERSTYLE_H

#include <QColor>

class QApplication;

namespace QuicksilverStyle {

enum class Token {
    Base,
    Surface,
    SurfaceRaised,
    Rail,
    Cinnabar,
    CinnabarBright,
    CinnabarDim,
    CinnabarTrace,
    Silver,
    SilverHi,
    SilverMuted,
    Teal,
    Amber,
    Violet,
};

QColor Color(Token token);
void Apply(QApplication& app);

} // namespace QuicksilverStyle

#endif // QUICKSILVER_QT_QUICKSILVERSTYLE_H
