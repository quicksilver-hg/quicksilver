// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/sendcoinsentry.h>
#include <qt/forms/ui_sendcoinsentry.h>

#include <chainparams.h>
#include <qt/addressbookpage.h>
#include <qt/addresstablemodel.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/quicksilverstyle.h>
#include <qt/vaultmodel.h>

#include <QApplication>
#include <QSizePolicy>
#include <QClipboard>
#include <QDialog>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QStyle>
#include <QStyleOption>

namespace {
//! The address-book label the Transfer test requires to show whole.
//! Nineteen characters. A different string of the same length is a different
//! width in a proportional font, so the reference is that label (F-493).
constexpr char LABEL_REFERENCE[] = "household-budget-02";

//! Outer width at which `field` paints `text` in full. The room is the same
//! sum the vault test uses: the style's contents rect, less 2 px a side and
//! the cursor. The frame and the sheet's padding are whatever that rect
//! leaves outside a probe width, so the result follows the font and the style.
int LineEditWidthForText(QLineEdit* field, const QString& text)
{
    field->ensurePolished();
    const int text_width = GUIUtil::TextWidth(field->fontMetrics(), text);
    QStyleOptionFrame option;
    option.initFrom(field);
    constexpr int probe = 1000;
    option.rect = QRect(0, 0, probe, qMax(field->fontMetrics().height(), 1));
    option.lineWidth = field->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &option, field);
    const int contents = field->style()->subElementRect(QStyle::SE_LineEditContents, &option, field).width();
    const int outside = contents > 0 && contents <= probe ? probe - contents : 0;
    return text_width + 2 * 2 + 1 + outside;
}
} // namespace


SendCoinsEntry::SendCoinsEntry(const PlatformStyle *_platformStyle, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::SendCoinsEntry),
    platformStyle(_platformStyle)
{
    ui->setupUi(this);

    ui->addressBookButton->setIcon(platformStyle->ColorIcon(":/icons/address-book", QuicksilverStyle::Color(QuicksilverStyle::Token::RailText)));
    ui->pasteButton->setIcon(platformStyle->ColorIcon(":/icons/editpaste", QuicksilverStyle::Color(QuicksilverStyle::Token::RailText)));

    // Two lines per recipient. The address and its commands take the first,
    // in the columns of the list's heading row: a whole Bech32 address does
    // not fit one line beside the amount and label at 1200 px. The amount and
    // label follow under the address, each named by a key: the amount as wide
    // as its figures, the label over the rest of the line and at least as wide
    // as an address-book label (F-452, F-493). The unit is the display unit,
    // named in the amount's key.
    ui->sendCoinsEntryIndex->setProperty("class", QStringLiteral("benchNote"));
    ui->sendCoinsEntryIndex->setFixedWidth(INDEX_WIDTH);
    ui->payAmount->setUnitSelectorVisible(false);
    ui->payAmount->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    ui->useAvailableBalanceButton->setFixedWidth(MAX_WIDTH);
    ui->useAvailableBalanceButton->setProperty("class", QStringLiteral("benchQuiet"));
    ui->addAsLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    ui->deleteButton->setFixedWidth(REMOVE_WIDTH);
    ui->deleteButton->setProperty("class", QStringLiteral("benchRemove"));
    QGridLayout* grid = ui->gridLayout;
    grid->removeItem(ui->horizontalLayoutAmount);
    grid->removeWidget(ui->addAsLabel);
    grid->removeWidget(ui->deleteButton);
    grid->removeItem(ui->messageLayout);
    grid->addWidget(ui->deleteButton, 0, 2);
    auto* amount_line = new QHBoxLayout;
    amount_line->setSpacing(SPACING);
    m_amount_key = new QLabel(this);
    m_amount_key->setObjectName(QStringLiteral("recipientAmountKey"));
    m_amount_key->setProperty("class", QStringLiteral("benchKey"));
    amount_line->addWidget(m_amount_key);
    ui->horizontalLayoutAmount->setParent(nullptr);
    amount_line->addLayout(ui->horizontalLayoutAmount);
    amount_line->addSpacing(2 * SPACING);
    auto* label_key = new QLabel(tr("Label"), this);
    label_key->setObjectName(QStringLiteral("recipientLabelKey"));
    label_key->setProperty("class", QStringLiteral("benchKey"));
    label_key->setBuddy(ui->addAsLabel);
    amount_line->addWidget(label_key);
    amount_line->addWidget(ui->addAsLabel, 1);
    grid->addLayout(amount_line, 1, 1, 1, 2);
    ui->messageLayout->setParent(nullptr);
    grid->addLayout(ui->messageLayout, 2, 1, 1, 2);
    grid->setHorizontalSpacing(SPACING);
    grid->setVerticalSpacing(SPACING);
    grid->setColumnStretch(1, 1);
    ui->messageLabel->hide();
    ui->messageTextLabel->hide();
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    GUIUtil::setupAddressWidget(ui->payTo, this);
    // The row's address column is narrow; the full example address the shared
    // setup puts in the placeholder only ever showed as "Enter a Quicksi…".
    ui->payTo->setPlaceholderText(tr("Address (%1…)").arg(QString::fromStdString(Params().Bech32HRP()) + QLatin1Char('1')));

    // The bench sheet matches QLineEdit, so a font set on a parent does not
    // arrive here by inheritance. FontChange and StyleChange on the field
    // itself (polish, or a later setFont) recompute the minimum.
    ui->addAsLabel->installEventFilter(this);
    updateLabelFieldMinimum();

    // Connect signals
    connect(ui->payAmount, &QuicksilverAmountField::valueChanged, this, &SendCoinsEntry::payAmountChanged);
    connect(ui->deleteButton, &QPushButton::clicked, this, &SendCoinsEntry::deleteClicked);
    connect(ui->useAvailableBalanceButton, &QPushButton::clicked, this, &SendCoinsEntry::useAvailableBalanceClicked);
}

SendCoinsEntry::~SendCoinsEntry()
{
    delete ui;
}

void SendCoinsEntry::on_pasteButton_clicked()
{
    // Paste text from clipboard into recipient field
    ui->payTo->setText(QApplication::clipboard()->text());
}

void SendCoinsEntry::on_addressBookButton_clicked()
{
    if(!model)
        return;
    auto dlg = new AddressBookPage(platformStyle, AddressBookPage::ForSelection, AddressBookPage::SendingTab, this);
    dlg->setModel(model->getAddressTableModel());
    connect(dlg, &QDialog::accepted, this, [this, dlg] {
        ui->payTo->setText(dlg->getReturnValue());
        ui->payAmount->setFocus();
    });
    GUIUtil::ShowModalDialogAsynchronously(dlg);
}

void SendCoinsEntry::on_payTo_textChanged(const QString &address)
{
    updateLabel(address);
}

void SendCoinsEntry::setModel(VaultModel *_model)
{
    this->model = _model;

    if (_model && _model->getOptionsModel())
        connect(_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &SendCoinsEntry::updateDisplayUnit);

    clear();
}

void SendCoinsEntry::clear()
{
    // clear UI elements for a normal transfer
    ui->payTo->clear();
    ui->addAsLabel->clear();
    ui->payAmount->clear();
    ui->messageTextLabel->clear();
    ui->messageTextLabel->hide();
    ui->messageLabel->hide();

    // update the display unit, to not use the default ("Hg")
    updateDisplayUnit();
}

void SendCoinsEntry::deleteClicked()
{
    Q_EMIT removeEntry(this);
}

void SendCoinsEntry::useAvailableBalanceClicked()
{
    Q_EMIT useAvailableBalance(this);
}

bool SendCoinsEntry::validate()
{
    if (!model)
        return false;

    // Check input validity
    bool retval = true;

    if (!model->validateAddress(ui->payTo->text()))
    {
        ui->payTo->setValid(false);
        retval = false;
    }

    if (!ui->payAmount->validate())
    {
        retval = false;
    }

    // Sending a zero amount is invalid
    if (ui->payAmount->value(nullptr) <= 0)
    {
        ui->payAmount->setValid(false);
        retval = false;
    }

    return retval;
}

SendCoinsRecipient SendCoinsEntry::getValue()
{
    recipient.address = ui->payTo->text();
    recipient.label = ui->addAsLabel->text();
    recipient.amount = ui->payAmount->value();
    recipient.message = ui->messageTextLabel->text();

    return recipient;
}

QWidget *SendCoinsEntry::setupTabChain(QWidget *prev)
{
    QWidget::setTabOrder(prev, ui->payTo);
    QWidget::setTabOrder(ui->payTo, ui->addressBookButton);
    QWidget::setTabOrder(ui->addressBookButton, ui->pasteButton);
    QWidget *w = ui->payAmount->setupTabChain(ui->pasteButton);
    QWidget::setTabOrder(w, ui->useAvailableBalanceButton);
    QWidget::setTabOrder(ui->useAvailableBalanceButton, ui->addAsLabel);
    QWidget::setTabOrder(ui->addAsLabel, ui->deleteButton);
    return ui->deleteButton;
}

void SendCoinsEntry::setValue(const SendCoinsRecipient &value)
{
    recipient = value;
    {
        // message
        ui->messageTextLabel->setText(recipient.message);
        ui->messageTextLabel->setVisible(!recipient.message.isEmpty());
        ui->messageLabel->setVisible(!recipient.message.isEmpty());

        ui->addAsLabel->clear();
        ui->payTo->setText(recipient.address); // this may set a label from addressbook
        if (!recipient.label.isEmpty()) // if a label had been set from the addressbook, don't overwrite with an empty label
            ui->addAsLabel->setText(recipient.label);
        ui->payAmount->setValue(recipient.amount);
    }
}

void SendCoinsEntry::setAddress(const QString &address)
{
    ui->payTo->setText(address);
    ui->payAmount->setFocus();
}

void SendCoinsEntry::setAmount(const CAmount &amount)
{
    ui->payAmount->setValue(amount);
}

bool SendCoinsEntry::isClear()
{
    return ui->payTo->text().isEmpty();
}

void SendCoinsEntry::setFocus()
{
    ui->payTo->setFocus();
}

void SendCoinsEntry::setIndex(int index)
{
    ui->sendCoinsEntryIndex->setText(QString::number(index));
}

void SendCoinsEntry::updateDisplayUnit()
{
    const QuicksilverUnit unit = model && model->getOptionsModel() ? model->getOptionsModel()->getDisplayUnit() : QuicksilverUnit::HGS;
    if (model && model->getOptionsModel()) {
        ui->payAmount->setDisplayUnit(unit);
    }
    m_amount_key->setText(tr("Amount") + QLatin1Char(' ') + QuicksilverUnits::shortName(unit));
}

void SendCoinsEntry::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::PaletteChange) {
        ui->addressBookButton->setIcon(platformStyle->ColorIcon(QStringLiteral(":/icons/address-book"), QuicksilverStyle::Color(QuicksilverStyle::Token::RailText)));
        ui->pasteButton->setIcon(platformStyle->ColorIcon(QStringLiteral(":/icons/editpaste"), QuicksilverStyle::Color(QuicksilverStyle::Token::RailText)));
    }
    if (e->type() == QEvent::FontChange || e->type() == QEvent::StyleChange) {
        updateLabelFieldMinimum();
    }

    QWidget::changeEvent(e);
}

bool SendCoinsEntry::eventFilter(QObject* object, QEvent* event)
{
    if (object == ui->addAsLabel && (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)) {
        updateLabelFieldMinimum();
    }
    return QWidget::eventFilter(object, event);
}

void SendCoinsEntry::updateLabelFieldMinimum()
{
    const int width = LineEditWidthForText(ui->addAsLabel, QString::fromLatin1(LABEL_REFERENCE));
    if (ui->addAsLabel->minimumWidth() == width) return;
    ui->addAsLabel->setMinimumWidth(width);
    ui->addAsLabel->updateGeometry();
}

bool SendCoinsEntry::updateLabel(const QString &address)
{
    if(!model)
        return false;

    // Fill in label from address book, if address has an associated label
    QString associatedLabel = model->getAddressTableModel()->labelForAddress(address);
    if(!associatedLabel.isEmpty())
    {
        ui->addAsLabel->setText(associatedLabel);
        return true;
    }

    return false;
}
