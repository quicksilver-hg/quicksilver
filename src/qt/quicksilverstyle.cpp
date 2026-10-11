// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/quicksilverstyle.h>

#include <QApplication>
#include <QPalette>
#include <QProxyStyle>
#include <QStyleFactory>

namespace QuicksilverStyle {

QColor Color(Token token)
{
    switch (token) {
    case Token::Base:
        return QColor(QStringLiteral("#101214"));
    case Token::Surface:
        return QColor(QStringLiteral("#171b1f"));
    case Token::SurfaceRaised:
        return QColor(QStringLiteral("#20262b"));
    case Token::Rail:
        return QColor(QStringLiteral("#0b0d0f"));
    case Token::Cinnabar:
        return QColor(QStringLiteral("#d84b3e"));
    case Token::CinnabarBright:
        return QColor(QStringLiteral("#ff7a6c"));
    case Token::CinnabarDim:
        return QColor(QStringLiteral("#6b3431"));
    case Token::CinnabarTrace:
        return QColor(QStringLiteral("#2d2222"));
    case Token::Silver:
        return QColor(QStringLiteral("#ccd5dc"));
    case Token::SilverHi:
        return QColor(QStringLiteral("#f5f7f9"));
    case Token::SilverMuted:
        return QColor(QStringLiteral("#8e9aa3"));
    case Token::Teal:
        return QColor(QStringLiteral("#9bd7c8"));
    case Token::Amber:
        return QColor(QStringLiteral("#f1cf7a"));
    case Token::Violet:
        return QColor(QStringLiteral("#c3ace8"));
    case Token::BenchSurface:
        return QColor(QStringLiteral("#15181c"));
    case Token::BenchSurfaceRaised:
        return QColor(QStringLiteral("#191d22"));
    case Token::BenchTop:
        return QColor(QStringLiteral("#14171b"));
    case Token::Hairline:
        return QColor(QStringLiteral("#252a30"));
    case Token::StatusGood:
        return QColor(QStringLiteral("#6cc4a1"));
    case Token::TickerText:
        return QColor(QStringLiteral("#c9d0d6"));
    case Token::RailText:
        return QColor(QStringLiteral("#aab3bb"));
    case Token::RailDisabled:
        return QColor(QStringLiteral("#4a525a"));
    case Token::StatePending:
        return QColor(QStringLiteral("#e0a84a"));
    case Token::CinnabarSoft:
        return QColor(QStringLiteral("#e98a7c"));
    case Token::BenchLabel:
        return QColor(QStringLiteral("#aab3bb"));
    case Token::Focus:
        return QColor(QStringLiteral("#8b959e"));
    case Token::Warning:
        return QColor(QStringLiteral("#e0a84a"));
    }
    return QColor(QStringLiteral("#ccd5dc"));
}

int FontPx(Type type)
{
    switch (type) {
    case Type::Caption:
        return 10;
    case Type::Label:
    case Type::Body:
    case Type::Value:
        return 12;
    case Type::Figure:
        return 14;
    case Type::Title:
        return 19;
    }
    return 12;
}

namespace {
class BenchStyle : public QProxyStyle
{
public:
    explicit BenchStyle(QStyle* base) : QProxyStyle(base)
    {
        // Kept so the style can be recreated by name.
        setObjectName(base->objectName());
    }

    int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget, QStyleHintReturn* data) const override
    {
        if (hint == SH_DialogButtonBox_ButtonsHaveIcons) return 0;
        return QProxyStyle::styleHint(hint, option, widget, data);
    }
};
} // namespace

QStyle* WithoutDialogButtonIcons(QStyle* base)
{
    return new BenchStyle(base);
}

void Apply(QApplication& app)
{
    // Native Windows widgets do not consistently honor dark palettes. Fusion
    // gives every supported platform the same paint path and stylesheet surface.
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        app.setStyle(WithoutDialogButtonIcons(fusion));
        app.setProperty("quicksilverBaseStyle", QStringLiteral("Fusion"));
    }

    QPalette palette;
    palette.setColor(QPalette::Window, Color(Token::Base));
    palette.setColor(QPalette::WindowText, Color(Token::Silver));
    palette.setColor(QPalette::Base, Color(Token::Surface));
    palette.setColor(QPalette::AlternateBase, Color(Token::Base));
    palette.setColor(QPalette::ToolTipBase, Color(Token::Surface));
    palette.setColor(QPalette::ToolTipText, Color(Token::SilverHi));
    palette.setColor(QPalette::Text, Color(Token::Silver));
    palette.setColor(QPalette::Button, Color(Token::Surface));
    palette.setColor(QPalette::ButtonText, Color(Token::Silver));
    palette.setColor(QPalette::BrightText, Color(Token::CinnabarBright));
    palette.setColor(QPalette::Link, Color(Token::CinnabarBright));
    palette.setColor(QPalette::LinkVisited, Color(Token::Cinnabar));
    palette.setColor(QPalette::Highlight, Color(Token::Cinnabar));
    palette.setColor(QPalette::HighlightedText, Color(Token::SilverHi));
    palette.setColor(QPalette::Disabled, QPalette::Base, Color(Token::Base));
    palette.setColor(QPalette::Disabled, QPalette::Button, Color(Token::Surface));
    palette.setColor(QPalette::Disabled, QPalette::Highlight, Color(Token::CinnabarTrace));
    palette.setColor(QPalette::Disabled, QPalette::Text, Color(Token::SilverMuted));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, Color(Token::SilverMuted));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, Color(Token::SilverMuted));
    app.setPalette(palette);

    QString sheet = QStringLiteral(R"(
QMainWindow,
QDialog,
QWidget#RPCConsole,
QWidget[class="quicksilverPage"] {
    background-color: #101214;
    color: #ccd5dc;
}
QMainWindow QWidget, QDialog QWidget {
    font-size: {body}px;
}
QToolTip {
    background-color: #20262b;
    border: 1px solid #6b3431;
    color: #f5f7f9;
    padding: 5px 7px;
}
QMenuBar {
    background-color: #0b0d0f;
    border: 0;
    border-bottom: 1px solid #2d2222;
    color: #ccd5dc;
    padding: 3px 8px;
    spacing: 4px;
}
QMenuBar::item {
    background: transparent;
    border-radius: 3px;
    padding: 5px 9px;
}
QMenuBar::item:selected {
    background-color: #20262b;
    color: #f5f7f9;
}
QMenu {
    background-color: #171b1f;
    border: 1px solid #39434b;
    color: #ccd5dc;
    padding: 5px;
}
QMenu::item {
    border-radius: 3px;
    padding: 6px 24px 6px 9px;
}
QMenu::item:selected {
    background-color: #6b3431;
    color: #f5f7f9;
}
QMenu::separator {
    background-color: #39434b;
    height: 1px;
    margin: 5px 7px;
}
QDialog QLabel {
    color: #ccd5dc;
}
QStatusBar {
    background-color: #0b0d0f;
    border-top: 1px solid #2d2222;
    color: #8e9aa3;
    min-height: 25px;
}
QStatusBar QLabel {
    color: #8e9aa3;
}
QToolBar#primaryCommandRail {
    background-color: #0b0d0f;
    border: 0;
    border-right: 1px solid #252a30;
    spacing: 2px;
    padding: 0 0 10px 0;
}
QFrame#commandRailBrand {
    background: transparent;
    border: 0;
    border-bottom: 1px solid #252a30;
    padding-bottom: 0;
}
QLabel#commandRailBrandTitle {
    color: #f5f7f9;
    font-size: {figure}px;
    font-weight: 700;
}
QLabel#commandRailSectionLabel {
    color: #5f6973;
    font-size: {caption}px;
    font-weight: 700;
}
QLabel#commandRailSectionLabel {
    padding: 12px 6px 4px 18px;
}
QToolBar#primaryCommandRail QToolButton {
    background: transparent;
    border: 0;
    border-left: 2px solid transparent;
    border-radius: 0;
    color: #aab3bb;
    min-height: 38px;
    max-height: 38px;
    padding-left: 16px;
    padding-right: 12px;
    text-align: left;
}
QToolBar#primaryCommandRail QToolButton[accent="cinnabar"] {
    color: #ff7a6c;
}
QToolBar#primaryCommandRail QToolButton[accent="teal"] {
    color: #9bd7c8;
}
QToolBar#primaryCommandRail QToolButton[accent="amber"] {
    color: #f1cf7a;
}
QToolBar#primaryCommandRail QToolButton[accent="violet"] {
    color: #c3ace8;
}
QToolBar#primaryCommandRail QToolButton[accent="silver"] {
    color: #ccd5dc;
}
QToolBar#primaryCommandRail QToolButton:checked {
    background-color: #171a1f;
    border-left: 2px solid #d84b3e;
    color: #f5f7f9;
}
QToolBar#primaryCommandRail QToolButton:hover {
    background-color: #171a1f;
    color: #f5f7f9;
}
QToolBar#primaryCommandRail QToolButton:disabled {
    color: #4a525a;
}
QComboBox,
QLineEdit,
QAbstractSpinBox,
QTextEdit,
QPlainTextEdit {
    background-color: #171b1f;
    border: 1px solid #39434b;
    border-radius: 4px;
    color: #f5f7f9;
    padding: 6px 8px;
    selection-background-color: #6b3431;
    selection-color: #f5f7f9;
}
QComboBox:focus,
QLineEdit:focus,
QAbstractSpinBox:focus,
QTextEdit:focus,
QPlainTextEdit:focus {
    border-color: {focus};
}
QAbstractSpinBox {
    padding-right: 0;
}
QAbstractSpinBox::up-button,
QAbstractSpinBox::down-button {
    background-color: transparent;
    border: 0;
    border-left: 1px solid #2c3238;
    subcontrol-origin: padding;
    width: 18px;
}
QAbstractSpinBox::up-button {
    subcontrol-position: top right;
}
QAbstractSpinBox::down-button {
    subcontrol-position: bottom right;
}
QAbstractSpinBox::up-button:hover,
QAbstractSpinBox::down-button:hover {
    background-color: #2a3037;
}
QAbstractSpinBox::up-arrow {
    image: url(:/icons/spin_up);
    height: 5px;
    width: 9px;
}
QAbstractSpinBox::down-arrow {
    image: url(:/icons/spin_down);
    height: 5px;
    width: 9px;
}
QAbstractSpinBox::up-arrow:disabled,
QAbstractSpinBox::up-arrow:off {
    image: url(:/icons/spin_up_off);
}
QAbstractSpinBox::down-arrow:disabled,
QAbstractSpinBox::down-arrow:off {
    image: url(:/icons/spin_down_off);
}
QComboBox:disabled,
QLineEdit:disabled,
QAbstractSpinBox:disabled,
QTextEdit:disabled,
QPlainTextEdit:disabled {
    background-color: #121518;
    border-color: #293138;
    color: #6f7a82;
}
QComboBox::drop-down {
    border: 0;
    width: 24px;
}
QComboBox QAbstractItemView {
    background-color: #171b1f;
    border: 1px solid #39434b;
    color: #ccd5dc;
    outline: 0;
    selection-background-color: #6b3431;
    selection-color: #f5f7f9;
}
QCheckBox,
QRadioButton {
    color: #ccd5dc;
    spacing: 8px;
}
QCheckBox:disabled,
QRadioButton:disabled {
    color: #6f7a82;
}
QCheckBox::indicator {
    background-color: #0f1114;
    border: 1px solid #8b959e;
    border-radius: 2px;
    height: 12px;
    width: 12px;
}
QCheckBox::indicator:hover {
    border-color: #aab3bb;
}
QCheckBox::indicator:checked {
    background-color: #2a3037;
    border-color: #aab3bb;
    image: url(:/icons/check_tick);
}
QCheckBox::indicator:disabled {
    border-color: #4a525a;
}
QWidget[class="quicksilverPage"] QGroupBox,
QFrame#coinControlPanel,
QFrame#sendWorkProgressPanel,
QFrame[class="consensusCostRow"],
QFrame#consensusChoicePanel {
    background-color: #171b1f;
    border: 1px solid #39434b;
    border-radius: 6px;
    padding: 12px;
}
QWidget[class="quicksilverPage"] QGroupBox {
    margin-top: 18px;
}
QWidget[class="quicksilverPage"] QGroupBox::title {
    color: #ff7a6c;
    subcontrol-origin: margin;
    left: 8px;
    padding: 0 4px;
}
QLabel[class="pageTitle"] {
    color: #f5f7f9;
    font-size: {title}px;
    font-weight: 700;
    padding: 4px 0;
}
QLabel[class="pageEyebrow"] {
    color: #ff7a6c;
    font-size: {caption}px;
    font-weight: 700;
}
QLabel[class="sectionValue"] {
    color: #f5f7f9;
    font-weight: 600;
}
QLabel[class="muted"] {
    color: #8e9aa3;
}
QLabel[class="policyReviewReady"],
QLabel[class="policyReviewValid"],
QLabel[class="policyReviewError"] {
    border-radius: 5px;
    padding: 6px 8px;
}
QLabel[class="policyReviewReady"] {
    background-color: #252d33;
    border: 1px solid #53606a;
    color: #ccd5dc;
}
QLabel[class="policyReviewValid"] {
    background-color: #21312e;
    border: 1px solid #4d7c69;
    color: #9bd7c8;
}
QLabel[class="policyReviewError"] {
    background-color: #2d2222;
    border: 1px solid #6b3431;
    color: #ff7a6c;
}
QLabel[class="hudHeading"] {
    color: #ff7a6c;
    font-size: {body}px;
    font-weight: 700;
}
QLabel[class="developerNetworkBanner"] {
    background-color: #2d2222;
    border: 1px solid #6b3431;
    border-radius: 5px;
    color: #f5f7f9;
    padding: 8px 10px;
}
QLabel[class="isolationBanner"] {
    background-color: #2d2222;
    border: 1px solid #6b3431;
    border-radius: 5px;
    color: #f5f7f9;
    padding: 8px 10px;
}
QFrame#consensusReviewHeader {
    background: transparent;
    border: 0;
}
QLabel#consensusReviewMark {
    background-color: #2d2222;
    border: 1px solid #6b3431;
    border-radius: 23px;
}
QLabel[class="launchCapabilityState"] {
    background-color: #21312e;
    border: 1px solid #4d7c69;
    border-radius: 4px;
    color: #9bd7c8;
    font-size: {label}px;
    font-weight: 700;
    padding: 4px 7px;
}
QFrame#consensusStorageCost {
    border-color: #53606a;
}
QFrame#consensusBackgroundCost {
    border-color: #8a7648;
}
QFrame#consensusVaultCost {
    border-color: #4d7c69;
}
QFrame#coinControlPanel,
QFrame#sendWorkProgressPanel {
    border-color: #6d5935;
}
QScrollArea#agentAllotmentScrollArea,
QWidget#agentAllotmentScrollContents {
    background-color: #101214;
    border: 0;
}
QScrollArea#tabMainScrollArea,
QWidget#tabMainScrollContents {
    background-color: #171b1f;
    border: 0;
}
QWidget#SendCoinsEntry,
QScrollArea#scrollArea,
QWidget#scrollAreaWidgetContents,
QFrame#frameCoinControl {
    background: transparent;
    border: 0;
}
QPushButton {
    background-color: #20262b;
    border: 1px solid #46515a;
    border-radius: 3px;
    color: #ccd5dc;
    min-height: 26px;
    padding: 0 12px;
}
QPushButton#sendButton,
QPushButton#receiveButton,
QPushButton[class="primaryActionButton"] {
    background-color: #6b3431;
    border-color: #d84b3e;
    color: #f5f7f9;
}
QPushButton[class="secondaryActionButton"] {
    background-color: #171b1f;
    border-color: #53606a;
    color: #ccd5dc;
}
QPushButton:disabled {
    background-color: #171b1f;
    color: #6f7a82;
    border-color: #293138;
}
QPushButton:hover:!disabled {
    background-color: #293138;
    border-color: #56606a;
}
QToolButton {
    background-color: #20262b;
    border: 1px solid #46515a;
    border-radius: 4px;
    color: #ccd5dc;
    min-height: 28px;
    min-width: 28px;
}
QToolButton:hover {
    background-color: #293138;
    border-color: #56606a;
}
Line {
    color: #39434b;
}
QAbstractItemView {
    alternate-background-color: #15191d;
    background-color: #101214;
    border: 1px solid #39434b;
    color: #ccd5dc;
    outline: 0;
    selection-background-color: #6b3431;
    selection-color: #f5f7f9;
}
QAbstractItemView::item {
    min-height: 22px;
}
QAbstractItemView::item:selected {
    background-color: #6b3431;
    color: #f5f7f9;
}
QHeaderView {
    background-color: #0b0d0f;
}
QHeaderView::section {
    background-color: #171b1f;
    border: 0;
    border-bottom: 1px solid #39434b;
    border-right: 1px solid #293138;
    color: #aeb9c1;
    padding: 7px;
}
QTabWidget::pane {
    background-color: #171b1f;
    border: 1px solid #39434b;
    border-radius: 4px;
    top: -1px;
}
QTabWidget > QWidget {
    background-color: #171b1f;
}
QTabBar {
    qproperty-drawBase: 0;
    background: transparent;
}
QTabBar::tab {
    background-color: #101214;
    border: 1px solid transparent;
    border-bottom: 2px solid transparent;
    color: #8e9aa3;
    min-width: 74px;
    padding: 8px 14px;
}
QTabBar::tab:hover {
    background-color: #20262b;
    color: #ccd5dc;
}
QTabBar::tab:selected {
    background-color: #171b1f;
    border-color: #39434b;
    border-bottom-color: #d84b3e;
    color: #f5f7f9;
}
QGroupBox {
    background-color: #171b1f;
    border: 1px solid #39434b;
    border-radius: 5px;
    color: #ccd5dc;
    margin-top: 14px;
    padding: 12px;
}
QGroupBox::title {
    color: #aeb9c1;
    subcontrol-origin: margin;
    left: 9px;
    padding: 0 4px;
}
QFrame#nodeWindowHeader {
    background-color: #171b1f;
    border: 0;
    border-bottom: 1px solid #39434b;
}
QScrollBar:vertical {
    background-color: #101214;
    width: 10px;
    margin: 0;
}
QScrollBar::handle:vertical {
    background-color: #46515a;
    border-radius: 4px;
    min-height: 24px;
}
QScrollBar:horizontal {
    background-color: #101214;
    height: 10px;
    margin: 0;
}
QScrollBar::handle:horizontal {
    background-color: #46515a;
    border-radius: 4px;
    min-width: 24px;
}
QScrollBar::add-line,
QScrollBar::sub-line,
QScrollBar::add-page,
QScrollBar::sub-page {
    background: transparent;
    border: 0;
}
QSplitter::handle {
    background-color: #39434b;
}
QProgressBar {
    background-color: #171b1f;
    border: 1px solid #39434b;
    border-radius: 4px;
    color: #f5f7f9;
    min-height: 16px;
    text-align: center;
}
QProgressBar::chunk {
    background-color: #6cc4a1;
    border-radius: 3px;
}
)");

    const QString bench_surface = Color(Token::BenchSurface).name();
    const QString bench_raised = Color(Token::BenchSurfaceRaised).name();
    const QString bench_top = Color(Token::BenchTop).name();
    const QString hairline = Color(Token::Hairline).name();
    const QString status_good = Color(Token::StatusGood).name();
    const QString ticker = Color(Token::TickerText).name();
    const QString hero = Color(Token::SilverHi).name();
    const QString muted = Color(Token::SilverMuted).name();
    const QString rail = Color(Token::Rail).name();
    const QString cinnabar = Color(Token::Cinnabar).name();
    sheet += QStringLiteral(R"(
QFrame#benchTopBar {
    background-color: %1;
    border: 0;
    border-bottom: 1px solid %2;
}
QFrame#benchPanel,
QFrame[benchPanel="true"] {
    background-color: %3;
    border: 1px solid %2;
    border-radius: 0;
}
QFrame#benchPanelHead {
    background-color: %4;
    border: 0;
    border-bottom: 1px solid %2;
}
QFrame#benchStatusStrip {
    background-color: %5;
    border: 0;
    border-top: 1px solid %2;
}
QLabel#benchBreadcrumb,
QLabel#benchTickerKey,
QLabel#benchPanelTitle,
QLabel#benchStatusText,
QLabel#benchTickerEmpty {
    color: %6;
}
QLabel#benchTickerSpendable {
    color: %7;
    font-size: {title}px;
}
QLabel#benchTickerPending,
QLabel#benchTickerMaturing,
QLabel#benchTickerDelegated,
QLabel#benchTickerTotal {
    color: %8;
    font-size: {figure}px;
}
QFrame#benchTickerSeparator {
    background-color: %2;
    border: 0;
}
QMenuBar {
    background-color: %1;
    border-bottom: 1px solid %2;
}
QLabel#benchStatusGood {
    color: %9;
    font-size: {label}px;
}
)").arg(bench_top, hairline, bench_surface, bench_raised, rail, muted, hero, ticker, status_good);
    sheet += QStringLiteral(R"(
QPushButton#sendButton,
QPushButton[class="primaryActionButton"] {
    background-color: %1;
    border-color: %1;
    color: %2;
}
QPushButton#sendButton:disabled,
QPushButton[class="primaryActionButton"]:disabled {
    background-color: #171b1f;
    border-color: #293138;
    color: #6f7a82;
}
QProgressBar {
    background-color: %3;
    border: 0;
    border-radius: 0;
    color: %2;
    min-height: 14px;
    max-height: 16px;
    text-align: center;
}
QProgressBar::chunk {
    background-color: %4;
    border-radius: 0;
}
QLabel#benchStatusSync,
QLabel#benchStatusHeight,
QLabel#benchStatusPeers,
QLabel#benchStatusMining,
QLabel#benchStatusVault {
    color: %5;
    font-size: {label}px;
}
QLabel#benchStatusSync[benchTone="good"],
QLabel#benchStatusDot[benchTone="good"] {
    color: %4;
}
QLabel#benchStatusDot {
    color: %5;
    font-size: {caption}px;
}
QLabel#benchTickerKey {
    color: %6;
    font-size: {caption}px;
    font-weight: 700;
}
QLabel#benchTickerUnit {
    color: %1;
    font-size: {label}px;
    font-weight: 700;
}
QLabel#benchBreadcrumb {
    font-size: {label}px;
}
QLabel#benchPanelTitle,
QLabel[class="benchSection"] {
    color: %6;
    font-size: {caption}px;
    font-weight: 700;
}
)").arg(cinnabar, hero, bench_surface, status_good, muted, Color(Token::RailText).name());
    sheet += QStringLiteral(R"(
QFrame#consensusChoicePanel,
QWidget#mineMintIsolationPanel {
    background-color: %1;
    border: 1px solid %2;
    border-radius: 0;
}
QFrame[benchBody="true"] {
    background: transparent;
    border: 0;
    padding: 0;
    margin: 0;
}
QPushButton#receiveButton,
QPushButton#consensusContinueButton,
QPushButton#startMiningButton,
QPushButton#agentAllotmentCreateButton {
    background-color: %4;
    border-color: %4;
    color: %3;
}
)").arg(bench_surface, hairline, hero, cinnabar);

    // The Bench page grammar: label/value rows, notes, quiet commands, the
    // segmented filter and the ledger table.
    sheet += QStringLiteral(R"(
QLabel[class="benchKey"] {
    color: {label_color};
}
QLabel[class="benchValue"] {
    color: %2;
}
QLabel[class="benchValue"][benchTone="good"] {
    color: %3;
}
QLabel[class="benchValue"][benchTone="warn"],
QLabel[class="benchNote"][benchTone="warn"] {
    color: %4;
}
QLabel[class="benchNote"] {
    color: {label_color};
    font-size: {label}px;
}
QPushButton[class="benchQuiet"] {
    background-color: transparent;
    border: 1px solid %5;
    color: %6;
}
QPushButton[class="benchQuiet"]:hover:!disabled {
    background-color: %7;
    border-color: %8;
    color: %2;
}
QPushButton[class="benchQuiet"]:disabled {
    background-color: transparent;
    border-color: %9;
    color: %10;
}
QPushButton[class="benchSegment"] {
    background-color: %11;
    border: 1px solid %5;
    border-radius: 0;
    color: %6;
    min-height: 16px;
    padding: 3px 10px;
    margin: 0;
}
QPushButton[class="benchSegment"]:checked {
    background-color: %12;
    border-color: %13;
    color: %2;
}
QTableView[class="benchTable"] {
    alternate-background-color: %14;
    background-color: %15;
    border: 0;
    border-top: 1px solid %9;
    selection-background-color: %16;
    selection-color: %2;
}
QTableView[class="benchTable"]::item {
    border: 0;
    padding: 0 6px;
}
QTableView[class="benchTable"] QHeaderView::section {
    background-color: %17;
    border: 0;
    border-bottom: 1px solid %9;
    color: %18;
    font-size: {caption}px;
    font-weight: 700;
    padding: 6px;
}
QLabel[class="benchColumnHead"] {
    color: %18;
    font-size: {caption}px;
    font-weight: 700;
}
QFrame#benchRule {
    background-color: %9;
    border: 0;
}
QToolButton[class="benchRemove"] {
    background-color: transparent;
    border: 1px solid %5;
    border-radius: 3px;
    color: %1;
    min-height: 24px;
    min-width: 24px;
}
QToolButton[class="benchRemove"]:hover {
    border-color: %8;
    color: %2;
}
QProgressBar#homeNodeSync {
    background-color: %19;
    border: 0;
    max-height: 4px;
    min-height: 4px;
}
)")
                 .arg(muted, hero, status_good, Color(Token::Warning).name(), QStringLiteral("#2c3238"), Color(Token::RailText).name(),
                      QStringLiteral("#1b1f24"), QStringLiteral("#56606a"), hairline)
                 .arg(Color(Token::RailDisabled).name(), QStringLiteral("#1b1f24"), QStringLiteral("#2a3037"), QStringLiteral("#3a4148"),
                      QStringLiteral("#171b20"), bench_surface, QStringLiteral("#2b2023"), bench_raised, QStringLiteral("#8b959e"))
                 .arg(QStringLiteral("#20252a"));

    sheet += QStringLiteral(R"(
QFrame#optionsPanel QTabWidget::pane {
    border: 0;
    border-radius: 0;
}
QFrame#optionsPanel QTabBar::tab {
    padding: 5px 10px;
    min-height: 26px;
    min-width: 0;
}
QFrame#optionsPanel QTabBar::tab:selected {
    border-color: transparent;
    border-bottom-color: %1;
}
QTextBrowser#aboutMessage {
    border: 0;
    background: transparent;
    padding: 0;
}
)").arg(Color(Token::Cinnabar).name());

    // One type scale and the shared label colour, filled in last so every
    // section above draws from the same values.
    const QList<QPair<QString, QString>> tokens{
        {QStringLiteral("{caption}"), QString::number(FontPx(Type::Caption))},
        {QStringLiteral("{label}"), QString::number(FontPx(Type::Label))},
        {QStringLiteral("{body}"), QString::number(FontPx(Type::Body))},
        {QStringLiteral("{value}"), QString::number(FontPx(Type::Value))},
        {QStringLiteral("{figure}"), QString::number(FontPx(Type::Figure))},
        {QStringLiteral("{title}"), QString::number(FontPx(Type::Title))},
        {QStringLiteral("{label_color}"), Color(Token::BenchLabel).name()},
        {QStringLiteral("{focus}"), Color(Token::Focus).name()},
    };
    for (const auto& [token, value] : tokens) sheet.replace(token, value);
    app.setStyleSheet(sheet);
}

} // namespace QuicksilverStyle
