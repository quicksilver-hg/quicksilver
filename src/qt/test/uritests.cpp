// Copyright (c) 2009-2018 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/uritests.h>

#include <qt/guiutil.h>
#include <qt/vaultmodel.h>

#include <QUrl>

void URITests::uriTests()
{
    SendCoinsRecipient rv;
    QUrl uri;
    const QString address = QStringLiteral("Sdi75iY4Nh5sWqimYrQgkDM9otbBeuqEQp");

    uri.setUrl(QString("quicksilver:%1?req-dontexist=").arg(address));
    QVERIFY(!GUIUtil::parseQuicksilverURI(uri, &rv));

    uri.setUrl(QString("quicksilver:%1?dontexist=").arg(address));
    QVERIFY(GUIUtil::parseQuicksilverURI(uri, &rv));
    QVERIFY(rv.address == address);
    QVERIFY(rv.label == QString());
    QVERIFY(rv.amount == 0);

    uri.setUrl(QString("quicksilver:%1?label=Quicksilver Example Address").arg(address));
    QVERIFY(GUIUtil::parseQuicksilverURI(uri, &rv));
    QVERIFY(rv.address == address);
    QVERIFY(rv.label == QString("Quicksilver Example Address"));
    QVERIFY(rv.amount == 0);

    uri.setUrl(QString("quicksilver:%1?amount=0.001").arg(address));
    QVERIFY(GUIUtil::parseQuicksilverURI(uri, &rv));
    QVERIFY(rv.address == address);
    QVERIFY(rv.label == QString());
    QVERIFY(rv.amount == 100000);

    uri.setUrl(QString("quicksilver:%1?amount=1.001").arg(address));
    QVERIFY(GUIUtil::parseQuicksilverURI(uri, &rv));
    QVERIFY(rv.address == address);
    QVERIFY(rv.label == QString());
    QVERIFY(rv.amount == 100100000);

    uri.setUrl(QString("quicksilver:%1?amount=100&label=Quicksilver Example").arg(address));
    QVERIFY(GUIUtil::parseQuicksilverURI(uri, &rv));
    QVERIFY(rv.address == address);
    QVERIFY(rv.amount == 10000000000LL);
    QVERIFY(rv.label == QString("Quicksilver Example"));

    uri.setUrl(QString("quicksilver:%1?message=Quicksilver Example Address").arg(address));
    QVERIFY(GUIUtil::parseQuicksilverURI(uri, &rv));
    QVERIFY(rv.address == address);
    QVERIFY(rv.label == QString());

    QVERIFY(GUIUtil::parseQuicksilverURI(QString("quicksilver:%1?message=Quicksilver Example Address").arg(address), &rv));
    QVERIFY(rv.address == address);
    QVERIFY(rv.label == QString());

    uri.setUrl(QString("quicksilver:%1?req-message=Quicksilver Example Address").arg(address));
    QVERIFY(GUIUtil::parseQuicksilverURI(uri, &rv));

    // Commas in amounts are not allowed.
    uri.setUrl(QString("quicksilver:%1?amount=1,000&label=Quicksilver Example").arg(address));
    QVERIFY(!GUIUtil::parseQuicksilverURI(uri, &rv));

    uri.setUrl(QString("quicksilver:%1?amount=1,000.0&label=Quicksilver Example").arg(address));
    QVERIFY(!GUIUtil::parseQuicksilverURI(uri, &rv));

    // There are two amount specifications. The last value wins.
    uri.setUrl(QString("quicksilver:%1?amount=100&amount=200&label=Quicksilver Example").arg(address));
    QVERIFY(GUIUtil::parseQuicksilverURI(uri, &rv));
    QVERIFY(rv.address == address);
    QVERIFY(rv.amount == 20000000000LL);
    QVERIFY(rv.label == QString("Quicksilver Example"));

    // The first amount value is correct. However, the second amount value is not valid. Hence, the URI is not valid.
    uri.setUrl(QString("quicksilver:%1?amount=100&amount=1,000&label=Quicksilver Example").arg(address));
    QVERIFY(!GUIUtil::parseQuicksilverURI(uri, &rv));

    // Test label containing a question mark ('?').
    uri.setUrl(QString("quicksilver:%1?amount=100&label=?").arg(address));
    QVERIFY(GUIUtil::parseQuicksilverURI(uri, &rv));
    QVERIFY(rv.address == address);
    QVERIFY(rv.amount == 10000000000LL);
    QVERIFY(rv.label == QString("?"));

    // Escape sequences are not supported.
    uri.setUrl(QString("quicksilver:%1?amount=100&label=%3F").arg(address));
    QVERIFY(GUIUtil::parseQuicksilverURI(uri, &rv));
    QVERIFY(rv.address == address);
    QVERIFY(rv.amount == 10000000000LL);
    QVERIFY(rv.label == QString("%3F"));
}
