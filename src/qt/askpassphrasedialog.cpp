// Copyright (c) 2011-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/askpassphrasedialog.h>
#include <qt/forms/ui_askpassphrasedialog.h>

#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/vaultmodel.h>

#include <support/allocators/secure.h>

#include <QKeyEvent>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>

AskPassphraseDialog::AskPassphraseDialog(Mode _mode, QWidget *parent, SecureString* passphrase_out) :
    QDialog(parent, GUIUtil::dialog_flags),
    ui(new Ui::AskPassphraseDialog),
    mode(_mode),
    m_passphrase_out(passphrase_out)
{
    ui->setupUi(this);

    ui->passEdit1->setMinimumSize(ui->passEdit1->sizeHint());
    ui->passEdit2->setMinimumSize(ui->passEdit2->sizeHint());
    ui->passEdit3->setMinimumSize(ui->passEdit3->sizeHint());

    ui->passEdit1->setMaxLength(MAX_PASSPHRASE_SIZE);
    ui->passEdit2->setMaxLength(MAX_PASSPHRASE_SIZE);
    ui->passEdit3->setMaxLength(MAX_PASSPHRASE_SIZE);

    // Setup Caps Lock detection.
    ui->passEdit1->installEventFilter(this);
    ui->passEdit2->installEventFilter(this);
    ui->passEdit3->installEventFilter(this);

    switch(mode)
    {
        case Encrypt: // Ask passphrase x2
            ui->warningLabel->setText(tr("Enter the new passphrase for the vault.<br/>Please use a passphrase of <b>ten or more random characters</b>, or <b>eight or more words</b>."));
            ui->passLabel1->hide();
            ui->passEdit1->hide();
            setWindowTitle(tr("Encrypt vault"));
            break;
        case Unlock: // Ask passphrase
            ui->warningLabel->setText(tr("This operation needs your vault passphrase to unlock the vault."));
            ui->passLabel2->hide();
            ui->passEdit2->hide();
            ui->passLabel3->hide();
            ui->passEdit3->hide();
            setWindowTitle(tr("Unlock vault"));
            break;
        case ChangePass: // Ask old passphrase + new passphrase x2
            setWindowTitle(tr("Change passphrase"));
            ui->warningLabel->setText(tr("Enter the old passphrase and new passphrase for the vault."));
            break;
    }
    textChanged();
    connect(ui->toggleShowPasswordButton, &QPushButton::toggled, this, &AskPassphraseDialog::toggleShowPassword);
    connect(ui->passEdit1, &QLineEdit::textChanged, this, &AskPassphraseDialog::textChanged);
    connect(ui->passEdit2, &QLineEdit::textChanged, this, &AskPassphraseDialog::textChanged);
    connect(ui->passEdit3, &QLineEdit::textChanged, this, &AskPassphraseDialog::textChanged);

    GUIUtil::handleCloseWindowShortcut(this);
}

AskPassphraseDialog::~AskPassphraseDialog()
{
    secureClearPassFields();
    delete ui;
}

void AskPassphraseDialog::setModel(VaultModel *_model)
{
    this->model = _model;
}

void AskPassphraseDialog::accept()
{
    SecureString oldpass, newpass1, newpass2;
    if (!model && mode != Encrypt)
        return;
    oldpass.reserve(MAX_PASSPHRASE_SIZE);
    newpass1.reserve(MAX_PASSPHRASE_SIZE);
    newpass2.reserve(MAX_PASSPHRASE_SIZE);

    oldpass.assign(std::string_view{ui->passEdit1->text().toStdString()});
    newpass1.assign(std::string_view{ui->passEdit2->text().toStdString()});
    newpass2.assign(std::string_view{ui->passEdit3->text().toStdString()});

    secureClearPassFields();

    switch(mode)
    {
    case Encrypt: {
        if(newpass1.empty() || newpass2.empty())
        {
            // Cannot encrypt with empty passphrase
            break;
        }
        auto* msgBoxConfirm = new QMessageBox(QMessageBox::Question,
                                  tr("Confirm vault encryption"),
                                  tr("Warning: If you encrypt your vault and lose your passphrase, you will <b>LOSE ALL OF YOUR Hg</b>!") + "<br><br>" + tr("Are you sure you wish to encrypt your vault?"),
                                  QMessageBox::Cancel | QMessageBox::Yes, this);
        msgBoxConfirm->setObjectName(QStringLiteral("encryptVaultConfirm"));
        msgBoxConfirm->button(QMessageBox::Yes)->setText(tr("Continue"));
        msgBoxConfirm->button(QMessageBox::Cancel)->setText(tr("Back"));
        msgBoxConfirm->setDefaultButton(QMessageBox::Cancel);
        GUIUtil::ShowModalMessageBoxAsynchronously(msgBoxConfirm, [this, newpass1, newpass2](int retval, QAbstractButton*) {
            if (retval != QMessageBox::Yes) return;
            finishEncrypt(newpass1, newpass2);
        });
    } break;
    case Unlock:
        try {
            if (!model->setVaultLocked(false, oldpass)) {
                auto* box = new QMessageBox(QMessageBox::Critical, tr("Vault unlock failed"),
                                      tr("The passphrase entered for the vault decryption was incorrect."),
                                      QMessageBox::Ok, this);
                box->setObjectName(QStringLiteral("unlockVaultFailed"));
                GUIUtil::ShowModalDialogAsynchronously(box);
            } else {
                if (m_passphrase_out) {
                    m_passphrase_out->assign(oldpass);
                }
                QDialog::accept(); // Success
            }
        } catch (const std::runtime_error& e) {
            auto* box = new QMessageBox(QMessageBox::Critical, tr("Vault unlock failed"), e.what(),
                                  QMessageBox::Ok, this);
            box->setObjectName(QStringLiteral("unlockVaultRuntimeError"));
            GUIUtil::ShowModalDialogAsynchronously(box);
        }
        break;
    case ChangePass:
        if(newpass1 == newpass2)
        {
            if(model->changePassphrase(oldpass, newpass1))
            {
                auto* box = new QMessageBox(QMessageBox::Information, tr("Vault encrypted"),
                                     tr("Vault passphrase was successfully changed."),
                                     QMessageBox::Ok, this);
                box->setObjectName(QStringLiteral("changePassSuccess"));
                QPointer<AskPassphraseDialog> self{this};
                GUIUtil::ShowModalMessageBoxAsynchronously(box, [self](int, QAbstractButton*) {
                    if (self) self->QDialog::accept();
                });
            }
            else
            {
                auto* box = new QMessageBox(QMessageBox::Critical, tr("Passphrase change failed"),
                                      tr("The passphrase entered for the vault decryption was incorrect."),
                                      QMessageBox::Ok, this);
                box->setObjectName(QStringLiteral("changePassFailed"));
                GUIUtil::ShowModalDialogAsynchronously(box);
            }
        }
        else
        {
            auto* box = new QMessageBox(QMessageBox::Critical, tr("Vault encryption failed"),
                                 tr("The supplied passphrases do not match."),
                                 QMessageBox::Ok, this);
            box->setObjectName(QStringLiteral("changePassMismatch"));
            GUIUtil::ShowModalDialogAsynchronously(box);
        }
        break;
    }
}

void AskPassphraseDialog::finishEncrypt(const SecureString& newpass1, const SecureString& newpass2)
{
    if (newpass1 != newpass2) {
        auto* box = new QMessageBox(QMessageBox::Critical, tr("Vault encryption failed"),
                             tr("The supplied passphrases do not match."), QMessageBox::Ok, this);
        box->setObjectName(QStringLiteral("encryptVaultMismatch"));
        GUIUtil::ShowModalDialogAsynchronously(box);
        return;
    }

    QString encryption_reminder = tr("Remember that encrypting your vault cannot fully protect "
    "your Hg from being stolen by malware infecting your computer.");
    if (m_passphrase_out) {
        m_passphrase_out->assign(newpass1);
        auto* msgBoxWarning = new QMessageBox(QMessageBox::Warning,
                                              tr("Vault to be encrypted"),
                                              "<qt>" +
                                                  tr("Your vault is about to be encrypted. ") + encryption_reminder + " " +
                                                  tr("Are you sure you wish to encrypt your vault?") +
                                                  "</b></qt>",
                                              QMessageBox::Cancel | QMessageBox::Yes, this);
        msgBoxWarning->setObjectName(QStringLiteral("encryptVaultReminder"));
        msgBoxWarning->setDefaultButton(QMessageBox::Cancel);
        GUIUtil::ShowModalMessageBoxAsynchronously(msgBoxWarning, [this](int retval, QAbstractButton*) {
            if (retval == QMessageBox::Cancel) {
                QDialog::reject();
                return;
            }
            QDialog::accept();
        });
        return;
    }

    assert(model != nullptr);
    QPointer<AskPassphraseDialog> self{this};
    auto accept_after = [self](int, QAbstractButton*) {
        if (self) self->QDialog::accept();
    };
    if (model->setVaultEncrypted(newpass1)) {
        auto* box = new QMessageBox(QMessageBox::Warning, tr("Vault encrypted"),
                             "<qt>" +
                             tr("Your vault is now encrypted. ") + encryption_reminder +
                             "<br><br><b>" +
                             tr("IMPORTANT: Any previous backups you have made of your vault file "
                             "should be replaced with the newly generated, encrypted vault file. "
                             "For security reasons, previous backups of the unencrypted vault file "
                             "will become useless as soon as you start using the new, encrypted vault.") +
                             "</b></qt>",
                             QMessageBox::Ok, this);
        box->setObjectName(QStringLiteral("encryptVaultSuccess"));
        GUIUtil::ShowModalMessageBoxAsynchronously(box, accept_after);
    } else {
        auto* box = new QMessageBox(QMessageBox::Critical, tr("Vault encryption failed"),
                             tr("Vault encryption failed due to an internal error. Your vault was not encrypted."),
                             QMessageBox::Ok, this);
        box->setObjectName(QStringLiteral("encryptVaultFailed"));
        GUIUtil::ShowModalMessageBoxAsynchronously(box, accept_after);
    }
}

void AskPassphraseDialog::textChanged()
{
    // Validate input, set Ok button to enabled when acceptable
    bool acceptable = false;
    switch(mode)
    {
    case Encrypt: // New passphrase x2
        acceptable = !ui->passEdit2->text().isEmpty() && !ui->passEdit3->text().isEmpty();
        break;
    case Unlock: // Old passphrase x1
        acceptable = !ui->passEdit1->text().isEmpty();
        break;
    case ChangePass: // Old passphrase x1, new passphrase x2
        acceptable = !ui->passEdit1->text().isEmpty() && !ui->passEdit2->text().isEmpty() && !ui->passEdit3->text().isEmpty();
        break;
    }
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(acceptable);
}

bool AskPassphraseDialog::event(QEvent *event)
{
    // Detect Caps Lock key press.
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_CapsLock) {
            fCapsLock = !fCapsLock;
        }
        if (fCapsLock) {
            ui->capsLabel->setText(tr("Warning: The Caps Lock key is on!"));
        } else {
            ui->capsLabel->clear();
        }
    }
    return QWidget::event(event);
}

void AskPassphraseDialog::toggleShowPassword(bool show)
{
    ui->toggleShowPasswordButton->setDown(show);
    const auto mode = show ? QLineEdit::Normal : QLineEdit::Password;
    ui->passEdit1->setEchoMode(mode);
    ui->passEdit2->setEchoMode(mode);
    ui->passEdit3->setEchoMode(mode);
}

bool AskPassphraseDialog::eventFilter(QObject *object, QEvent *event)
{
    /* Detect Caps Lock.
     * There is no good OS-independent way to check a key state in Qt, but we
     * can detect Caps Lock by checking for the following condition:
     * Shift key is down and the result is a lower case character, or
     * Shift key is not down and the result is an upper case character.
     */
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        QString str = ke->text();
        if (str.length() != 0) {
            const QChar *psz = str.unicode();
            bool fShift = (ke->modifiers() & Qt::ShiftModifier) != 0;
            if ((fShift && *psz >= 'a' && *psz <= 'z') || (!fShift && *psz >= 'A' && *psz <= 'Z')) {
                fCapsLock = true;
                ui->capsLabel->setText(tr("Warning: The Caps Lock key is on!"));
            } else if (psz->isLetter()) {
                fCapsLock = false;
                ui->capsLabel->clear();
            }
        }
    }
    return QDialog::eventFilter(object, event);
}

static void SecureClearQLineEdit(QLineEdit* edit)
{
    // Attempt to overwrite text so that they do not linger around in memory
    edit->setText(QString(" ").repeated(edit->text().size()));
    edit->clear();
}

void AskPassphraseDialog::secureClearPassFields()
{
    SecureClearQLineEdit(ui->passEdit1);
    SecureClearQLineEdit(ui->passEdit2);
    SecureClearQLineEdit(ui->passEdit3);
}
