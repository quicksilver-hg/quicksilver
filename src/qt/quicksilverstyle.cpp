// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/quicksilverstyle.h>

#include <QApplication>
#include <QPalette>
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
    }
    return QColor(QStringLiteral("#ccd5dc"));
}

void Apply(QApplication& app)
{
    // Native Windows widgets do not consistently honor dark palettes. Fusion
    // gives every supported platform the same paint path and stylesheet surface.
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        app.setStyle(fusion);
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

    app.setStyleSheet(QStringLiteral(R"(
QMainWindow,
QDialog,
QWidget#RPCConsole,
QWidget[class="quicksilverPage"] {
    background-color: #101214;
    color: #ccd5dc;
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
    border-right: 1px solid #2d2222;
    spacing: 3px;
    padding: 12px 9px 10px 9px;
}
QFrame#commandRailBrand {
    background: transparent;
    border: 0;
    border-bottom: 1px solid #2d2222;
    margin-bottom: 9px;
    padding-bottom: 12px;
}
QLabel#commandRailBrandTitle {
    color: #f5f7f9;
    font-size: 13px;
    font-weight: 700;
}
QLabel#commandRailSectionLabel {
    color: #8e9aa3;
    font-size: 10px;
    font-weight: 600;
}
QLabel#commandRailSectionLabel {
    padding: 3px 6px 6px 6px;
}
QToolBar#primaryCommandRail QToolButton {
    background: transparent;
    border: 1px solid transparent;
    border-radius: 5px;
    color: #ccd5dc;
    min-height: 38px;
    min-width: 132px;
    padding: 6px 9px;
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
    background-color: #2d2222;
    border-color: #d84b3e;
    color: #f5f7f9;
}
QToolBar#primaryCommandRail QToolButton:hover {
    background-color: #20262b;
    border-color: #6b3431;
    color: #f5f7f9;
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
    border-color: #d84b3e;
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
QCheckBox::indicator,
QRadioButton::indicator {
    background-color: #101214;
    border: 1px solid #53606a;
    height: 14px;
    width: 14px;
}
QCheckBox::indicator {
    border-radius: 3px;
}
QRadioButton::indicator {
    border-radius: 8px;
}
QCheckBox::indicator:checked,
QRadioButton::indicator:checked {
    background-color: #d84b3e;
    border-color: #ff7a6c;
}
QCheckBox:disabled,
QRadioButton:disabled {
    color: #6f7a82;
}
QWidget[class="quicksilverPage"] QGroupBox,
QFrame#overviewBalancePanel,
QFrame#overviewActivityPanel,
QFrame#overviewBackupPanel,
QFrame#frameCoinControl,
QFrame#receiveRequestPanel,
QFrame#receiveHistoryPanel,
QFrame#coinControlPanel,
QFrame#sendWorkProgressPanel,
QFrame#agentAllotmentRiskPanel,
QFrame#agentAllotmentAcceptancePanel,
QFrame#agentAllotmentRecordsPanel,
QFrame#agentAllotmentPolicyReviewPanel,
QFrame#agentAllotmentPaymentReceiptPanel,
QFrame#agentAllotmentSpendCommandPanel,
QFrame#agentAllotmentSignedSpendPanel,
QFrame#mineMintPayoutPanel,
QFrame[class="launchCapabilityCard"],
QFrame#desktopLaunchBackupPanel,
QFrame#desktopLaunchCostPanel,
QFrame[class="consensusCostRow"],
QFrame[class="networkContextCard"],
QFrame#consensusChoicePanel {
    background-color: #171b1f;
    border: 1px solid #39434b;
    border-radius: 6px;
    padding: 12px;
}
QWidget[class="quicksilverPage"] QGroupBox,
QFrame#overviewBalancePanel,
QFrame#overviewActivityPanel,
QFrame#overviewBackupPanel {
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
    font-size: 21px;
    font-weight: 700;
    padding: 4px 0;
}
QLabel[class="pageEyebrow"] {
    color: #ff7a6c;
    font-size: 10px;
    font-weight: 700;
}
QLabel[class="sectionValue"] {
    color: #f5f7f9;
    font-weight: 600;
}
QLabel[class="muted"] {
    color: #8e9aa3;
}
QLabel[class="policyReviewIdle"],
QLabel[class="policyReviewReady"],
QLabel[class="policyReviewValid"],
QLabel[class="policyReviewError"] {
    border-radius: 5px;
    padding: 6px 8px;
}
QLabel[class="policyReviewIdle"] {
    background-color: #101214;
    border: 1px solid #39434b;
    color: #8e9aa3;
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
    font-size: 12px;
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
QLabel[class="hudValue"] {
    color: #f5f7f9;
    font-weight: 700;
}
QFrame#desktopLaunchHeader {
    background: transparent;
    border: 0;
}
QLabel#desktopLaunchMark {
    background-color: #2d2222;
    border: 1px solid #6b3431;
    border-radius: 24px;
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
QLabel[class="launchCapabilityTitle"] {
    color: #f5f7f9;
    font-size: 16px;
    font-weight: 700;
}
QLabel#launchVaultCardTitle {
    color: #ff7a6c;
}
QLabel#launchConsensusCardTitle {
    color: #9bd7c8;
}
QLabel#launchMiningCardTitle {
    color: #f1cf7a;
}
QLabel[class="launchCapabilityState"] {
    background-color: #21312e;
    border: 1px solid #4d7c69;
    border-radius: 4px;
    color: #9bd7c8;
    font-size: 11px;
    font-weight: 700;
    padding: 4px 7px;
}
QLabel#launchMiningCardState,
QLabel#desktopLaunchBackupState {
    background-color: #322a1c;
    border-color: #8a7648;
    color: #f1cf7a;
}
QLabel#launchConsensusCardState {
    background-color: #2d2222;
    border-color: #6b3431;
    color: #ff7a6c;
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
QFrame#agentAllotmentRiskPanel {
    border-color: #8a7648;
}
QFrame#agentAllotmentAcceptancePanel {
    border-color: #6b3431;
}
QFrame#launchVaultCard {
    border-color: #6b3431;
}
QFrame#launchConsensusCard,
QFrame#receiveRequestPanel,
QFrame#receiveHistoryPanel,
QFrame[class="networkContextCard"] {
    border-color: #365f5a;
}
QFrame#launchMiningCard,
QFrame#frameCoinControl,
QFrame#coinControlPanel,
QFrame#sendWorkProgressPanel,
QFrame#mineMintPayoutPanel {
    border-color: #6d5935;
}
QFrame#agentAllotmentRecordsPanel,
QFrame#agentAllotmentPolicyReviewPanel,
QFrame#agentAllotmentPaymentReceiptPanel,
QFrame#agentAllotmentSpendCommandPanel,
QFrame#agentAllotmentSignedSpendPanel {
    border-color: #514567;
}
QScrollArea#agentAllotmentScrollArea,
QWidget#agentAllotmentScrollContents {
    background-color: #101214;
    border: 0;
}
QWidget#SendCoinsEntry {
    background-color: #0b0d0f;
    border: 1px solid #39434b;
    border-radius: 6px;
}
QScrollArea#scrollArea {
    background-color: #101214;
    border: 0;
}
QWidget#scrollAreaWidgetContents {
    background-color: #101214;
}
QPushButton {
    background-color: #20262b;
    border: 1px solid #46515a;
    border-radius: 4px;
    color: #ccd5dc;
    min-height: 19px;
    padding: 6px 13px;
}
QPushButton#sendButton,
QPushButton#receiveButton,
QPushButton#emptyVaultCreateButton,
QPushButton[class="launchPrimaryButton"],
QPushButton[class="primaryActionButton"] {
    background-color: #6b3431;
    border-color: #d84b3e;
    color: #f5f7f9;
    font-weight: 700;
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
    border-color: #ff7a6c;
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
    border-color: #d84b3e;
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
QFrame#noVaultState {
    background-color: #171b1f;
    border: 1px solid #39434b;
    border-radius: 6px;
}
QLabel#emptyVaultMark {
    background-color: #2d2222;
    border: 1px solid #6b3431;
    border-radius: 24px;
    color: #ff7a6c;
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
    background-color: #d84b3e;
    border-radius: 3px;
}
)"));
}

} // namespace QuicksilverStyle
