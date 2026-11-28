// Copyright (c) 2015-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_PLATFORMSTYLE_H
#define QUICKSILVER_QT_PLATFORMSTYLE_H

#include <QIcon>
#include <QPixmap>
#include <QString>

/* Platform identity and icon recoloring. */
class PlatformStyle
{
public:
    /** Get style associated with provided platform name, or nullptr for an unknown platform */
    static const PlatformStyle *instantiate(const QString &platformId);

    const QString &getName() const { return name; }


    QColor TextColor() const;
    QColor SingleColor() const;

    /** Colorize an image (given filename) with the icon color */
    QImage SingleColorImage(const QString& filename) const;

    /** Colorize an icon (given filename) with the icon color */
    QIcon SingleColorIcon(const QString& filename) const;

    /** Colorize an icon (given object) with the text color */
    QIcon TextColorIcon(const QIcon& icon) const;

    /** Colorize an icon with an explicit application accent color. */
    QIcon ColorIcon(const QString& filename, const QColor& color) const;

private:
    explicit PlatformStyle(const QString& platform_name);

    QString name;
};

#endif // QUICKSILVER_QT_PLATFORMSTYLE_H
