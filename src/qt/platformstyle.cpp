// Copyright (c) 2015-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platformstyle.h>

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QPalette>

namespace {
/* Local functions for colorizing single-color images */

void MakeSingleColorImage(QImage& img, const QColor& colorbase)
{
    img = img.convertToFormat(QImage::Format_ARGB32);
    for (int x = img.width(); x--; )
    {
        for (int y = img.height(); y--; )
        {
            const QRgb rgb = img.pixel(x, y);
            img.setPixel(x, y, qRgba(colorbase.red(), colorbase.green(), colorbase.blue(), qAlpha(rgb)));
        }
    }
}

QIcon ColorizeIcon(const QIcon& ico, const QColor& colorbase)
{
    QIcon new_ico;
    for (const QSize& sz : ico.availableSizes())
    {
        QImage img(ico.pixmap(sz).toImage());
        MakeSingleColorImage(img, colorbase);
        new_ico.addPixmap(QPixmap::fromImage(img));
    }
    return new_ico;
}

QImage ColorizeImage(const QString& filename, const QColor& colorbase)
{
    QImage img(filename);
    MakeSingleColorImage(img, colorbase);
    return img;
}

QIcon ColorizeIcon(const QString& filename, const QColor& colorbase)
{
    return QIcon(QPixmap::fromImage(ColorizeImage(filename, colorbase)));
}

}


PlatformStyle::PlatformStyle(const QString& platform_name) : name(platform_name)
{
}

QColor PlatformStyle::TextColor() const
{
    return QApplication::palette().color(QPalette::WindowText);
}

QColor PlatformStyle::SingleColor() const
{
    QColor colorHighlightBg(QApplication::palette().color(QPalette::Highlight));
    QColor colorHighlightFg(QApplication::palette().color(QPalette::HighlightedText));
    const QColor colorText(QApplication::palette().color(QPalette::WindowText));
    const int colorTextLightness = colorText.lightness();
    if (abs(colorHighlightBg.lightness() - colorTextLightness) < abs(colorHighlightFg.lightness() - colorTextLightness)) {
        return colorHighlightBg;
    }
    return colorHighlightFg;
}

QImage PlatformStyle::SingleColorImage(const QString& filename) const
{
    return ColorizeImage(filename, SingleColor());
}

QIcon PlatformStyle::SingleColorIcon(const QString& filename) const
{
    return ColorizeIcon(filename, SingleColor());
}

QIcon PlatformStyle::TextColorIcon(const QIcon& icon) const
{
    return ColorizeIcon(icon, TextColor());
}

QIcon PlatformStyle::ColorIcon(const QString& filename, const QColor& color) const
{
    return ColorizeIcon(filename, color);
}

const PlatformStyle* PlatformStyle::instantiate(const QString& platformId)
{
    if (platformId == QStringLiteral("windows") || platformId == QStringLiteral("other")) {
        return new PlatformStyle(platformId);
    }
    return nullptr;
}
