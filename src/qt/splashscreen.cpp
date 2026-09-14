// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <quicksilver-build-config.h> // IWYU pragma: keep

#include <qt/splashscreen.h>

#include <clientversion.h>
#include <common/system.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <interfaces/vault.h>
#include <qt/guiutil.h>
#include <qt/networkstyle.h>
#include <qt/quicksilverstyle.h>
#include <qt/vaultmodel.h>
#include <util/translation.h>

#include <functional>

#include <QApplication>
#include <QCloseEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPen>
#include <QScreen>


SplashScreen::SplashScreen(const NetworkStyle* networkStyle)
    : QWidget()
{
    // set reference point, paddings
    int paddingRight            = 44;
    int paddingTop              = 56;
    int titleVersionVSpace      = 17;
    int titleCopyrightVSpace    = 48;

    float fontFactor            = 1.0;
    float devicePixelRatio      = 1.0;
    devicePixelRatio = static_cast<QGuiApplication*>(QCoreApplication::instance())->devicePixelRatio();

    // define text to place
    QString titleText       = CLIENT_NAME;
    QString versionText     = QString("Version %1").arg(QString::fromStdString(FormatFullVersion()));
    QString copyrightText   = QString::fromUtf8(CopyrightHolders("\xc2\xA9 ").c_str());
    const QString& titleAddText    = networkStyle->getTitleAddText();

    QString font            = QApplication::font().toString();

    // create a bitmap according to device pixelratio
    const QSize logicalSize(560, 360);
    QSize splashSize(logicalSize.width()*devicePixelRatio, logicalSize.height()*devicePixelRatio);
    pixmap = QPixmap(splashSize);

    // change to HiDPI if it makes sense
    pixmap.setDevicePixelRatio(devicePixelRatio);

    QPainter pixPaint(&pixmap);
    pixPaint.setRenderHint(QPainter::Antialiasing);

    QLinearGradient gradient(QPoint(0, 0), QPoint(logicalSize.width(), logicalSize.height()));
    gradient.setColorAt(0, QuicksilverStyle::Color(QuicksilverStyle::Token::Base));
    gradient.setColorAt(0.58, QuicksilverStyle::Color(QuicksilverStyle::Token::Surface));
    gradient.setColorAt(1, QuicksilverStyle::Color(QuicksilverStyle::Token::SurfaceRaised));
    QRect rGradient(QPoint(0,0), logicalSize);
    pixPaint.fillRect(rGradient, gradient);

    pixPaint.setPen(QPen(QColor(210, 64, 45, 34), 1));
    for (int x = 24; x < logicalSize.width(); x += 28) {
        pixPaint.drawLine(x, 0, x, logicalSize.height());
    }
    for (int y = 24; y < logicalSize.height(); y += 28) {
        pixPaint.drawLine(0, y, logicalSize.width(), y);
    }

    pixPaint.setPen(QPen(QuicksilverStyle::Color(QuicksilverStyle::Token::Cinnabar), 2));
    pixPaint.drawLine(32, 36, 222, 36);
    pixPaint.drawLine(32, 36, 32, 246);
    pixPaint.setPen(QPen(QColor(QStringLiteral("#c9483a")), 2));
    pixPaint.drawLine(logicalSize.width() - 44, 78, logicalSize.width() - 44, 286);
    pixPaint.drawLine(logicalSize.width() - 218, 286, logicalSize.width() - 44, 286);

    // Draw the Quicksilver icon as a large HUD watermark and a sharp foreground mark.
    QRect rectIcon(QPoint(38, 74), QSize(190, 190));

    const QSize requiredSize(1024,1024);
    QPixmap icon(networkStyle->getAppIcon().pixmap(requiredSize));

    pixPaint.setOpacity(0.22);
    pixPaint.drawPixmap(rectIcon, icon);
    pixPaint.setOpacity(1.0);
    pixPaint.drawPixmap(QRect(QPoint(74, 110), QSize(118, 118)), icon);

    // check font size and drawing with
    pixPaint.setFont(QFont(font, 33*fontFactor));
    QFontMetrics fm = pixPaint.fontMetrics();
    const QString displayTitle = titleText.toUpper();
    int titleTextWidth = GUIUtil::TextWidth(fm, displayTitle);
    if (titleTextWidth > 250) {
        fontFactor = fontFactor * 250 / titleTextWidth;
    }

    pixPaint.setFont(QFont(font, 33*fontFactor));
    fm = pixPaint.fontMetrics();
    titleTextWidth  = GUIUtil::TextWidth(fm, displayTitle);
    pixPaint.setPen(QuicksilverStyle::Color(QuicksilverStyle::Token::SilverHi));
    pixPaint.drawText(logicalSize.width()-titleTextWidth-paddingRight,paddingTop,displayTitle);

    pixPaint.setFont(QFont(font, 15*fontFactor));

    // if the version string is too long, reduce size
    fm = pixPaint.fontMetrics();
    int versionTextWidth  = GUIUtil::TextWidth(fm, versionText);
    if(versionTextWidth > titleTextWidth+paddingRight-10) {
        pixPaint.setFont(QFont(font, 10*fontFactor));
        titleVersionVSpace -= 5;
    }
    pixPaint.setPen(QuicksilverStyle::Color(QuicksilverStyle::Token::CinnabarBright));
    pixPaint.drawText(logicalSize.width()-titleTextWidth-paddingRight+2,paddingTop+titleVersionVSpace,versionText);

    pixPaint.setFont(QFont(font, 10*fontFactor, QFont::Bold));
    pixPaint.setPen(QuicksilverStyle::Color(QuicksilverStyle::Token::Cinnabar));
    pixPaint.drawText(logicalSize.width()-titleTextWidth-paddingRight+2,paddingTop+titleVersionVSpace+24,QStringLiteral("QUICKSILVER NODE HUD"));

    // draw copyright stuff
    {
        pixPaint.setFont(QFont(font, 10*fontFactor));
        pixPaint.setPen(QuicksilverStyle::Color(QuicksilverStyle::Token::SilverMuted));
        const int x = logicalSize.width()-titleTextWidth-paddingRight;
        const int y = paddingTop+titleCopyrightVSpace;
        QRect copyrightRect(x, y, logicalSize.width() - x - paddingRight, logicalSize.height() - y);
        pixPaint.drawText(copyrightRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, copyrightText);
    }

    pixPaint.setPen(QPen(QuicksilverStyle::Color(QuicksilverStyle::Token::CinnabarDim), 1));
    pixPaint.setBrush(Qt::NoBrush);
    pixPaint.drawRoundedRect(QRect(18, 18, logicalSize.width() - 36, logicalSize.height() - 36), 8, 8);

    // draw additional text if special network
    if(!titleAddText.isEmpty()) {
        QFont boldFont = QFont(font, 10*fontFactor);
        boldFont.setWeight(QFont::Bold);
        pixPaint.setFont(boldFont);
        fm = pixPaint.fontMetrics();
        int titleAddTextWidth  = GUIUtil::TextWidth(fm, titleAddText);
        pixPaint.setPen(QuicksilverStyle::Color(QuicksilverStyle::Token::Cinnabar));
        pixPaint.drawText(logicalSize.width()-titleAddTextWidth-18,18,titleAddText);
    }

    pixPaint.end();

    // Set window title
    setWindowTitle(titleText + " " + titleAddText);

    // Resize window and move to center of desktop, disallow resizing
    QRect r(QPoint(), QSize(pixmap.size().width()/devicePixelRatio,pixmap.size().height()/devicePixelRatio));
    resize(r.size());
    setFixedSize(r.size());
    move(QGuiApplication::primaryScreen()->geometry().center() - r.center());

    installEventFilter(this);

    GUIUtil::handleCloseWindowShortcut(this);
}

SplashScreen::~SplashScreen()
{
    if (m_node) unsubscribeFromCoreSignals();
}

void SplashScreen::setNode(interfaces::Node& node)
{
    assert(!m_node);
    m_node = &node;
    subscribeToCoreSignals();
    if (m_shutdown) m_node->startShutdown();
}

void SplashScreen::shutdown()
{
    m_shutdown = true;
    if (m_node) m_node->startShutdown();
}

bool SplashScreen::eventFilter(QObject * obj, QEvent * ev) {
    if (ev->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(ev);
        if (keyEvent->key() == Qt::Key_Q) {
            shutdown();
        }
    }
    return QObject::eventFilter(obj, ev);
}

static void InitMessage(SplashScreen *splash, const std::string &message)
{
    bool invoked = QMetaObject::invokeMethod(splash, "showMessage",
        Qt::QueuedConnection,
        Q_ARG(QString, QString::fromStdString(message)),
        Q_ARG(int, Qt::AlignBottom|Qt::AlignHCenter),
        Q_ARG(QColor, QuicksilverStyle::Color(QuicksilverStyle::Token::CinnabarBright)));
    assert(invoked);
}

static void ShowProgress(SplashScreen *splash, const std::string &title, int nProgress, bool resume_possible)
{
    InitMessage(splash, title + std::string("\n") +
            (resume_possible ? SplashScreen::tr("(press q to shutdown and continue later)").toStdString()
                                : SplashScreen::tr("press q to shutdown").toStdString()) +
            strprintf("\n%d", nProgress) + "%");
}

void SplashScreen::subscribeToCoreSignals()
{
    // Connect signals to client
    m_handler_init_message = m_node->handleInitMessage(std::bind(InitMessage, this, std::placeholders::_1));
    m_handler_show_progress = m_node->handleShowProgress(std::bind(ShowProgress, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    m_handler_init_vault = m_node->handleInitVault([this]() { handleLoadVault(); });
}

void SplashScreen::handleLoadVault()
{
#ifdef ENABLE_VAULT
    if (!VaultModel::isVaultEnabled()) return;
    m_handler_load_vault = m_node->vaultLoader().handleLoadVault([this](std::unique_ptr<interfaces::Vault> vault) {
        m_connected_vault_handlers.emplace_back(vault->handleShowProgress(std::bind(ShowProgress, this, std::placeholders::_1, std::placeholders::_2, false)));
        m_connected_vaults.emplace_back(std::move(vault));
    });
#endif
}

void SplashScreen::unsubscribeFromCoreSignals()
{
    // Disconnect signals from client
    m_handler_init_message->disconnect();
    m_handler_show_progress->disconnect();
    for (const auto& handler : m_connected_vault_handlers) {
        handler->disconnect();
    }
    m_connected_vault_handlers.clear();
    m_connected_vaults.clear();
}

void SplashScreen::showMessage(const QString &message, int alignment, const QColor &color)
{
    curMessage = message;
    curAlignment = alignment;
    curColor = color;
    update();
}

void SplashScreen::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.drawPixmap(0, 0, pixmap);
    QRect r = rect().adjusted(5, 5, -5, -5);
    painter.setPen(curColor);
    painter.drawText(r, curAlignment, curMessage);
}

void SplashScreen::closeEvent(QCloseEvent *event)
{
    shutdown(); // allows an "emergency" shutdown during startup
    event->ignore();
}
