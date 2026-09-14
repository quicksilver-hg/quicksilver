// Copyright (c) 2011-2020 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_QUICKSILVERADDRESSVALIDATOR_H
#define QUICKSILVER_QT_QUICKSILVERADDRESSVALIDATOR_H

#include <QValidator>

/** Base58 entry widget validator, checks for valid characters and
 * removes some whitespace.
 */
class QuicksilverAddressEntryValidator : public QValidator
{
    Q_OBJECT

public:
    explicit QuicksilverAddressEntryValidator(QObject *parent);

    State validate(QString &input, int &pos) const override;
};

/** Quicksilver address widget validator, checks for a valid Quicksilver address.
 */
class QuicksilverAddressCheckValidator : public QValidator
{
    Q_OBJECT

public:
    explicit QuicksilverAddressCheckValidator(QObject *parent);

    State validate(QString &input, int &pos) const override;
};

#endif // QUICKSILVER_QT_QUICKSILVERADDRESSVALIDATOR_H
