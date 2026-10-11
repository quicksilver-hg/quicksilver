// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_QUICKSILVERSTYLE_H
#define QUICKSILVER_QT_QUICKSILVERSTYLE_H

#include <QColor>

class QApplication;
class QStyle;

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
    // Assay Bench roles the earlier palette does not name. A concept colour that
    // sits within a shade of a token above keeps that token.
    BenchSurface,
    BenchSurfaceRaised,
    BenchTop,
    Hairline,
    StatusGood,
    TickerText,
    // Rail row text and icon tint, and the same for a destination that is
    // not available yet.
    RailText,
    RailDisabled,
    // Ledger state: a transfer waiting for a block, and an outgoing amount or
    // a maturing reward.
    StatePending,
    CinnabarSoft,
    // A row's key and the notes under it: as legible as the values they name.
    BenchLabel,
    // The border of the focused input. Not cinnabar, which reads as an error.
    Focus,
    // A caution: something to know or do, not a failure. Shares the pending
    // amber; cinnabar is kept for errors.
    Warning,
};

//! The one type scale. Every font size the application stylesheet sets is one
//! of these, so a caption, a label and the value it names keep the same
//! relation on every page.
enum class Type {
    Caption, //!< upper-case panel titles, ticker keys, column heads
    Label,   //!< the key in a label/value row, notes, the status strip
    Body,    //!< inputs, buttons, running text
    Value,   //!< the value in a label/value row
    Figure,  //!< ticker figures
    Title,   //!< the spendable figure and page titles
};

int FontPx(Type type);
QColor Color(Token token);
void Apply(QApplication& app);
//! Takes ownership of base. A desktop theme can ask for a stock icon on every
//! dialog button (the GNOME theme does, under GNOME and Cinnamon); Bench
//! buttons are their words, so the returned style never puts one there.
QStyle* WithoutDialogButtonIcons(QStyle* base);

} // namespace QuicksilverStyle

#endif // QUICKSILVER_QT_QUICKSILVERSTYLE_H
