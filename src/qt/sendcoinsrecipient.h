// Copyright (c) 2011-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QUICKSILVER_QT_SENDCOINSRECIPIENT_H
#define QUICKSILVER_QT_SENDCOINSRECIPIENT_H

#include <consensus/amount.h>
#include <serialize.h>

#include <string>

#include <QString>

class SendCoinsRecipient
{
public:
    explicit SendCoinsRecipient() : amount(0), nVersion(SendCoinsRecipient::CURRENT_VERSION) { }
    explicit SendCoinsRecipient(const QString &addr, const QString &_label, const CAmount& _amount, const QString &_message):
        address(addr), label(_label), amount(_amount), message(_message), nVersion(SendCoinsRecipient::CURRENT_VERSION) {}

    QString address;
    QString label;
    CAmount amount;
    QString message;

    static const int CURRENT_VERSION = 1;
    int nVersion;

    SERIALIZE_METHODS(SendCoinsRecipient, obj)
    {
        std::string address_str, label_str, message_str;

        SER_WRITE(obj, address_str = obj.address.toStdString());
        SER_WRITE(obj, label_str = obj.label.toStdString());
        SER_WRITE(obj, message_str = obj.message.toStdString());

        READWRITE(obj.nVersion, address_str, label_str, obj.amount, message_str);

        SER_READ(obj, obj.address = QString::fromStdString(address_str));
        SER_READ(obj, obj.label = QString::fromStdString(label_str));
        SER_READ(obj, obj.message = QString::fromStdString(message_str));
    }
};

#endif // QUICKSILVER_QT_SENDCOINSRECIPIENT_H
