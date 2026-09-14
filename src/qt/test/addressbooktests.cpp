// Copyright (c) 2017-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/addressbooktests.h>
#include <qt/test/util.h>
#include <test/util/setup_common.h>

#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <qt/addressbookpage.h>
#include <qt/clientmodel.h>
#include <qt/editaddressdialog.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/vaultmodel.h>

#include <addresstype.h>
#include <key.h>
#include <key_io.h>
#include <vault/vault.h>
#include <vault/test/util.h>
#include <vaultinitinterface.h>

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableView>

using vault::AddVault;
using vault::CVault;
using vault::CreateMockableVaultDatabase;
using vault::RemoveVault;
using vault::VAULT_FLAG_DESCRIPTORS;
using vault::VaultContext;

namespace
{

/**
 * Fill the edit address dialog box with data, submit it, and ensure that
 * the resulting message meets expectations.
 */
void EditAddressAndSubmit(
        EditAddressDialog* dialog,
        const QString& label, const QString& address, QString expected_msg)
{
    dialog->findChild<QLineEdit*>("labelEdit")->setText(label);
    dialog->findChild<QValidatedLineEdit*>("addressEdit")->setText(address);
    dialog->accept();

    if (expected_msg.isEmpty()) {
        QApplication::processEvents();
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            QVERIFY2(!(widget->inherits("QMessageBox") && widget->isVisible()),
                     "unexpected message box on successful address add");
        }
        return;
    }

    QWidget* modal = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((modal = QApplication::activeModalWidget()) && modal->inherits("QMessageBox"), 1000);
    auto* box = qobject_cast<QMessageBox*>(modal);
    QVERIFY(box);
    QCOMPARE(box->text(), expected_msg);
    QVERIFY(box->defaultButton());
    box->defaultButton()->click();
}

/**
 * Test adding various send addresses to the address book.
 *
 * There are three cases tested:
 *
 *   - new_address: a new address which should add as a send address successfully.
 *   - existing_s_address: an existing sending address which won't add successfully.
 *   - existing_r_address: an existing receiving address which won't add successfully.
 *
 * In each case, verify the resulting state of the address book and optionally
 * the warning message presented to the user.
 */
void TestAddAddressesToSendBook(interfaces::Node& node)
{
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    node.setContext(&test.m_node);
    const std::shared_ptr<CVault> vault = std::make_shared<CVault>(node.context()->chain.get(), "", CreateMockableVaultDatabase());
    vault->LoadVault();
    vault->SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
    {
        LOCK(vault->cs_vault);
        vault->SetupDescriptorScriptPubKeyMans();
    }

    auto build_address = [&vault]() {
        CKey key = GenerateRandomKey();
        CTxDestination dest{WitnessV0KeyHash(key.GetPubKey())};

        return std::make_pair(dest, QString::fromStdString(EncodeDestination(dest)));
    };

    CTxDestination r_key_dest, s_key_dest;

    // Add a preexisting "receive" entry in the address book.
    QString preexisting_r_address;
    QString r_label("already here (r)");

    // Add a preexisting "send" entry in the address book.
    QString preexisting_s_address;
    QString s_label("already here (s)");

    // Define a new address (which should add to the address book successfully).
    QString new_address_a;
    QString new_address_b;

    std::tie(r_key_dest, preexisting_r_address) = build_address();
    std::tie(s_key_dest, preexisting_s_address) = build_address();
    std::tie(std::ignore, new_address_a) = build_address();
    std::tie(std::ignore, new_address_b) = build_address();

    {
        LOCK(vault->cs_vault);
        vault->SetAddressBook(r_key_dest, r_label.toStdString(), vault::AddressPurpose::RECEIVE);
        vault->SetAddressBook(s_key_dest, s_label.toStdString(), vault::AddressPurpose::SEND);
    }

    auto check_addbook_size = [&vault](int expected_size) {
        LOCK(vault->cs_vault);
        QCOMPARE(static_cast<int>(vault->m_address_book.size()), expected_size);
    };

    // We should start with the two addresses we added earlier and nothing else.
    check_addbook_size(2);

    // Initialize relevant QT models.
    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    OptionsModel optionsModel(node);
    bilingual_str error;
    QVERIFY(optionsModel.Init(error));
    ClientModel clientModel(node, &optionsModel);
    VaultContext& context = *node.vaultLoader().context();
    AddVault(context, vault);
    VaultModel vaultModel(interfaces::MakeVault(context, vault), clientModel, platformStyle.get());
    RemoveVault(context, vault, /* load_on_start= */ std::nullopt);
    EditAddressDialog editAddressDialog(EditAddressDialog::NewSendingAddress);
    editAddressDialog.setModel(vaultModel.getAddressTableModel());

    AddressBookPage address_book{platformStyle.get(), AddressBookPage::ForEditing, AddressBookPage::SendingTab};
    address_book.setModel(vaultModel.getAddressTableModel());
    auto table_view = address_book.findChild<QTableView*>("tableView");
    QCOMPARE(table_view->model()->rowCount(), 1);

    AddressBookPage receiving_book{platformStyle.get(), AddressBookPage::ForEditing, AddressBookPage::ReceivingTab};
    receiving_book.setModel(vaultModel.getAddressTableModel());
    QCOMPARE(receiving_book.findChild<QLabel*>(QStringLiteral("labelExplanation"))->text(),
             QStringLiteral("These are your Quicksilver receiving addresses. Use the 'Generate receiving address' button in the request tab to create new addresses.\nMessage signing is only possible with Base58 addresses."));

    EditAddressAndSubmit(
        &editAddressDialog, QString("uhoh"), preexisting_r_address,
        QString(
            "Address \"%1\" already exists as a receiving address with label "
            "\"%2\" and so cannot be added as a sending address."
            ).arg(preexisting_r_address).arg(r_label));
    check_addbook_size(2);
    QCOMPARE(table_view->model()->rowCount(), 1);

    EditAddressAndSubmit(
        &editAddressDialog, QString("uhoh, different"), preexisting_s_address,
        QString(
            "The entered address \"%1\" is already in the address book with "
            "label \"%2\"."
            ).arg(preexisting_s_address).arg(s_label));
    check_addbook_size(2);
    QCOMPARE(table_view->model()->rowCount(), 1);

    // Submit a new address which should add successfully - we expect the
    // warning message to be blank.
    EditAddressAndSubmit(
        &editAddressDialog, QString("io - new A"), new_address_a, QString(""));
    check_addbook_size(3);
    QCOMPARE(table_view->model()->rowCount(), 2);

    EditAddressAndSubmit(
        &editAddressDialog, QString("io - new B"), new_address_b, QString(""));
    check_addbook_size(4);
    QCOMPARE(table_view->model()->rowCount(), 3);

    auto search_line = address_book.findChild<QLineEdit*>("searchLineEdit");

    search_line->setText(r_label);
    QCOMPARE(table_view->model()->rowCount(), 0);

    search_line->setText(s_label);
    QCOMPARE(table_view->model()->rowCount(), 1);

    search_line->setText("io");
    QCOMPARE(table_view->model()->rowCount(), 2);

    // Check wildcard "?".
    search_line->setText("io?new");
    QCOMPARE(table_view->model()->rowCount(), 0);
    search_line->setText("io???new");
    QCOMPARE(table_view->model()->rowCount(), 2);

    // Check wildcard "*".
    search_line->setText("io*new");
    QCOMPARE(table_view->model()->rowCount(), 2);
    search_line->setText("*");
    QCOMPARE(table_view->model()->rowCount(), 3);

    search_line->setText(preexisting_r_address);
    QCOMPARE(table_view->model()->rowCount(), 0);

    search_line->setText(preexisting_s_address);
    QCOMPARE(table_view->model()->rowCount(), 1);

    search_line->setText(new_address_a);
    QCOMPARE(table_view->model()->rowCount(), 1);

    search_line->setText(new_address_b);
    QCOMPARE(table_view->model()->rowCount(), 1);

    search_line->setText("");
    QCOMPARE(table_view->model()->rowCount(), 3);
}

} // namespace

void AddressBookTests::addressBookTests()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        // Disable for mac on "minimal" platform to avoid crashes inside the Qt
        // framework when it tries to look up unimplemented cocoa functions,
        // and fails to handle returned nulls
        // (https://bugreports.qt.io/browse/QTBUG-49686).
        qWarning() << "Skipping AddressBookTests on mac build with 'minimal' platform set due to Qt bugs. To run AppTests, invoke "
                      "with 'QT_QPA_PLATFORM=cocoa test_quicksilver-qt' on mac, or else use a linux or windows build.";
        return;
    }
#endif
    TestAddAddressesToSendBook(m_node);
}

void AddressBookTests::newSendingAddressDoesNotNestEventLoop()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        qWarning() << "Skipping AddressBookTests on mac build with 'minimal' platform set due to Qt bugs.";
        return;
    }
#endif
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    m_node.setContext(&test.m_node);
    const std::shared_ptr<CVault> vault = std::make_shared<CVault>(m_node.context()->chain.get(), "", CreateMockableVaultDatabase());
    vault->LoadVault();
    vault->SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
    {
        LOCK(vault->cs_vault);
        vault->SetupDescriptorScriptPubKeyMans();
    }

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    OptionsModel optionsModel(m_node);
    bilingual_str error;
    QVERIFY(optionsModel.Init(error));
    ClientModel clientModel(m_node, &optionsModel);
    VaultContext& context = *m_node.vaultLoader().context();
    AddVault(context, vault);
    VaultModel vaultModel(interfaces::MakeVault(context, vault), clientModel, platformStyle.get());
    RemoveVault(context, vault, /* load_on_start= */ std::nullopt);

    AddressBookPage address_book{platformStyle.get(), AddressBookPage::ForEditing, AddressBookPage::SendingTab};
    address_book.setModel(vaultModel.getAddressTableModel());
    QPushButton* new_address = address_book.findChild<QPushButton*>(QStringLiteral("newAddress"));
    QVERIFY(new_address);
    ExpectModalWithoutNestedEventLoop("EditAddressDialog", [&] {
        new_address->click();
    });
}

void AddressBookTests::exportDoesNotNestEventLoop()
{
#ifdef Q_OS_MACOS
    if (QApplication::platformName() == "minimal") {
        qWarning() << "Skipping AddressBookTests on mac build with 'minimal' platform set due to Qt bugs.";
        return;
    }
#endif
    TestChain100Setup test;
    auto vault_loader = interfaces::MakeVaultLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.vault_loader = vault_loader.get();
    m_node.setContext(&test.m_node);
    const std::shared_ptr<CVault> vault = std::make_shared<CVault>(m_node.context()->chain.get(), "", CreateMockableVaultDatabase());
    vault->LoadVault();
    vault->SetVaultFlag(VAULT_FLAG_DESCRIPTORS);
    {
        LOCK(vault->cs_vault);
        vault->SetupDescriptorScriptPubKeyMans();
    }

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    OptionsModel optionsModel(m_node);
    bilingual_str error;
    QVERIFY(optionsModel.Init(error));
    ClientModel clientModel(m_node, &optionsModel);
    VaultContext& context = *m_node.vaultLoader().context();
    AddVault(context, vault);
    VaultModel vaultModel(interfaces::MakeVault(context, vault), clientModel, platformStyle.get());
    RemoveVault(context, vault, /* load_on_start= */ std::nullopt);

    AddressBookPage address_book{platformStyle.get(), AddressBookPage::ForEditing, AddressBookPage::SendingTab};
    address_book.setModel(vaultModel.getAddressTableModel());
    QPushButton* export_button = address_book.findChild<QPushButton*>(QStringLiteral("exportButton"));
    QVERIFY(export_button);
    QVERIFY(!export_button->isHidden());
    ExpectModalWithoutNestedEventLoop("QFileDialog", [&] {
        export_button->click();
    });
}
