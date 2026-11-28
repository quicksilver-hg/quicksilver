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
#include <sync.h>
#include <util/translation.h>

#include <list>

#include <QApplication>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPen>
#include <QScreen>


struct SplashScreen::CoreSignalState {
    Mutex mutex;
    bool shutting_down GUARDED_BY(mutex){false};
    std::unique_ptr<interfaces::Handler> load_vault_handler GUARDED_BY(mutex);
    std::list<std::unique_ptr<interfaces::Vault>> vaults GUARDED_BY(mutex);
    std::list<std::unique_ptr<interfaces::Handler>> vault_handlers GUARDED_BY(mutex);
};

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

    const QString font = QApplication::font().family();

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

    pixPaint.setPen(QPen(QuicksilverStyle::Color(QuicksilverStyle::Token::CinnabarTrace), 1));
    for (int x = 24; x < logicalSize.width(); x += 28) {
        pixPaint.drawLine(x, 0, x, logicalSize.height());
    }
    for (int y = 24; y < logicalSize.height(); y += 28) {
        pixPaint.drawLine(0, y, logicalSize.width(), y);
    }

    pixPaint.setPen(QPen(QuicksilverStyle::Color(QuicksilverStyle::Token::Vermilion), 2));
    pixPaint.drawLine(32, 36, 222, 36);
    pixPaint.drawLine(32, 36, 32, 246);
    pixPaint.setPen(QPen(QuicksilverStyle::Color(QuicksilverStyle::Token::Vermilion), 2));
    pixPaint.drawLine(logicalSize.width() - 44, 78, logicalSize.width() - 44, 286);
    pixPaint.drawLine(logicalSize.width() - 218, 286, logicalSize.width() - 44, 286);

    // Draw the Quicksilver icon as a large watermark and a sharp foreground mark.
    QRect rectIcon(QPoint(38, 74), QSize(190, 190));

    const QSize requiredSize(1024,1024);
    QPixmap icon(networkStyle->getAppIcon().pixmap(requiredSize));

    pixPaint.setOpacity(0.22);
    pixPaint.drawPixmap(rectIcon, icon);
    pixPaint.setOpacity(1.0);
    pixPaint.drawPixmap(QRect(QPoint(74, 110), QSize(118, 118)), icon);

    // Fit the title within its available width.
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
    pixPaint.setPen(QuicksilverStyle::Color(QuicksilverStyle::Token::Vermilion));
    pixPaint.drawText(logicalSize.width()-titleTextWidth-paddingRight+2,paddingTop+titleVersionVSpace+24,tr("Vault and node").toUpper());

    // draw copyright stuff
    {
        pixPaint.setFont(QFont(font, 10*fontFactor));
        pixPaint.setPen(QuicksilverStyle::Color(QuicksilverStyle::Token::SilverMuted));
        const int x = logicalSize.width()-titleTextWidth-paddingRight;
        const int y = paddingTop+titleCopyrightVSpace;
        QRect copyrightRect(x, y, logicalSize.width() - x - paddingRight, logicalSize.height() - y);
        pixPaint.drawText(copyrightRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, copyrightText);
    }

    pixPaint.setPen(QPen(QuicksilverStyle::Color(QuicksilverStyle::Token::CinnabarFill), 1));
    pixPaint.setBrush(Qt::NoBrush);
    pixPaint.drawRoundedRect(QRect(18, 18, logicalSize.width() - 36, logicalSize.height() - 36), 8, 8);

    // draw additional text if special network
    if(!titleAddText.isEmpty()) {
        QFont boldFont = QFont(font, 10*fontFactor);
        boldFont.setWeight(QFont::Bold);
        pixPaint.setFont(boldFont);
        fm = pixPaint.fontMetrics();
        int titleAddTextWidth  = GUIUtil::TextWidth(fm, titleAddText);
        pixPaint.setPen(QuicksilverStyle::Color(QuicksilverStyle::Token::Vermilion));
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
    // Signals2 disconnects future calls but does not wait for a running slot.
    // The shared state lets a slot finish even if the splash has been destroyed.
    m_core_signals = std::make_shared<CoreSignalState>();
    const auto state = m_core_signals;
    m_handler_init_message = m_node->handleInitMessage([this, state](const std::string& message) {
        LOCK(state->mutex);
        if (!state->shutting_down) InitMessage(this, message);
    });
    m_handler_show_progress = m_node->handleShowProgress([this, state](const std::string& title, int progress, bool resume_possible) {
        LOCK(state->mutex);
        if (!state->shutting_down) ShowProgress(this, title, progress, resume_possible);
    });
#ifdef ENABLE_VAULT
    m_handler_init_vault = m_node->handleInitVault([this, state, node = m_node] {
        if (!VaultModel::isVaultEnabled()) return;
        auto load_handler = node->vaultLoader().handleLoadVault([this, state](std::unique_ptr<interfaces::Vault> vault) {
            auto progress_handler = vault->handleShowProgress([this, state](const std::string& title, int progress) {
                LOCK(state->mutex);
                if (!state->shutting_down) ShowProgress(this, title, progress, false);
            });
            LOCK(state->mutex);
            if (!state->shutting_down) {
                state->vaults.emplace_back(std::move(vault));
                state->vault_handlers.emplace_back(std::move(progress_handler));
            }
        });
        {
            LOCK(state->mutex);
            if (!state->shutting_down && !state->load_vault_handler) {
                state->load_vault_handler = std::move(load_handler);
            }
        }
        if (load_handler) load_handler->disconnect();
    });
#endif
}

void SplashScreen::unsubscribeFromCoreSignals()
{
    std::unique_ptr<interfaces::Handler> load_vault_handler;
    {
        LOCK(m_core_signals->mutex);
        m_core_signals->shutting_down = true;
        load_vault_handler = std::move(m_core_signals->load_vault_handler);
    }

    m_handler_init_message->disconnect();
    m_handler_show_progress->disconnect();
    if (m_handler_init_vault) m_handler_init_vault->disconnect();

    // The vault load callback acquires the splash mutex with vaults_mutex
    // held. Disconnect without the splash mutex and wait out that callback.
    if (load_vault_handler) load_vault_handler->disconnect();
    std::list<std::unique_ptr<interfaces::Vault>> vaults;
    std::list<std::unique_ptr<interfaces::Handler>> vault_handlers;
    {
        LOCK(m_core_signals->mutex);
        vaults.splice(vaults.end(), m_core_signals->vaults);
        vault_handlers.splice(vault_handlers.end(), m_core_signals->vault_handlers);
    }
    for (const auto& handler : vault_handlers) handler->disconnect();
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
