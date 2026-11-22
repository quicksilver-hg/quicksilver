// Copyright (c) 2011-2022 The Bitcoin Core developers
// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/paymentserver.h>

#include <qt/quicksilverunits.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>

#include <chainparams.h>
#include <common/args.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <logging.h>
#include <node/interface_ui.h>
#include <policy/policy.h>
#include <vault/vault.h>

#include <cstdlib>
#include <memory>

#include <QApplication>
#include <QByteArray>
#include <QDataStream>
#include <QDebug>
#include <QFile>
#include <QFileOpenEvent>
#include <QHash>
#include <QList>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <QStringList>
#include <QUrlQuery>

const int QUICKSILVER_IPC_CONNECT_TIMEOUT = 1000; // milliseconds
const QString QUICKSILVER_IPC_PREFIX("quicksilver:");

//
// Create a name that is unique for:
//  selected network / default network
//  data directory
//
static QString ipcServerName()
{
    QString name("Quicksilver");

    // Append a simple hash of the datadir
    // Note that gArgs.GetDataDirNet() returns a different path
    // for a non-default network versus the default network
    QString ddir(GUIUtil::PathToQString(gArgs.GetDataDirNet()));
    name.append(QString::number(qHash(ddir)));

    return name;
}

//
// We store payment URIs and requests received before
// the main GUI window is up and ready to ask the user
// to send payment.

static QSet<QString> savedPaymentRequests;

PaymentServerFrame::Status PaymentServerFrame::decode(const QByteArray& bytes, QString* uri, const char** reason)
{
    if (bytes.size() < 4) return Status::Incomplete;
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.constData());
    const quint32 length = (quint32(data[0]) << 24) | (quint32(data[1]) << 16) | (quint32(data[2]) << 8) | quint32(data[3]);
    auto invalid = [reason](const char* why) {
        if (reason) *reason = why;
        return Status::Invalid;
    };
    if (length == 0xffffffffU || (length & 1U)) return invalid("malformed");
    if (length > 64U * 1024U) return invalid("too large");
    const qsizetype expected = 4 + length;
    if (bytes.size() > expected) return invalid("malformed");
    if (bytes.size() < expected) return Status::Incomplete;
    QDataStream stream(bytes);
    stream.setVersion(QDataStream::Qt_4_0);
    QString decoded;
    stream >> decoded;
    if (stream.status() != QDataStream::Ok || decoded.isNull()) return invalid("malformed");
    if (uri) *uri = decoded;
    return Status::Complete;
}

//
// Sending to the server is done synchronously, at startup.
// If the server isn't already running, startup continues,
// and the items in savedPaymentRequests will be handled
// when uiReady() is called.
//
// Warning: ipcSendCommandLine() is called early in init,
// so don't use "Q_EMIT message()", but "QMessageBox::"!
//
void PaymentServer::ipcParseCommandLine(int argc, char* argv[])
{
    for (int i = 1; i < argc; i++)
    {
        QString arg(argv[i]);
        if (arg.startsWith("-")) continue;

        if (arg.startsWith(QUICKSILVER_IPC_PREFIX, Qt::CaseInsensitive)) // quicksilver: URI
        {
            savedPaymentRequests.insert(arg);
        }
    }
}

//
// Sending to the server is done synchronously, at startup.
// If the server isn't already running, startup continues,
// and the items in savedPaymentRequests will be handled
// when uiReady() is called.
//
bool PaymentServer::ipcSendCommandLine()
{
    bool fResult = false;
    for (const QString& r : savedPaymentRequests)
    {
        QLocalSocket* socket = new QLocalSocket();
        socket->connectToServer(ipcServerName(), QIODevice::WriteOnly);
        if (!socket->waitForConnected(QUICKSILVER_IPC_CONNECT_TIMEOUT))
        {
            delete socket;
            socket = nullptr;
            return false;
        }

        QByteArray block;
        QDataStream out(&block, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_4_0);
        out << r;
        out.device()->seek(0);

        const bool written = socket->write(block) == block.size() &&
            (socket->bytesToWrite() == 0 || socket->waitForBytesWritten(QUICKSILVER_IPC_CONNECT_TIMEOUT));
        socket->disconnectFromServer();

        delete socket;
        socket = nullptr;
        if (!written) return false;
        fResult = true;
    }

    return fResult;
}

PaymentServer::PaymentServer(QObject* parent, bool startLocalServer)
    : QObject(parent)
{
    // Install global event filter to catch QFileOpenEvents
    // on Mac: sent when you click quicksilver: links
    // other OSes: helpful when dealing with payment request files
    if (parent)
        parent->installEventFilter(this);

    QString name = ipcServerName();

    if (startLocalServer)
    {
        QLocalSocket probe;
        probe.connectToServer(name, QIODevice::WriteOnly);
        if (probe.waitForConnected(QUICKSILVER_IPC_CONNECT_TIMEOUT)) {
            probe.disconnectFromServer();
            LogInfo(HgLog::QT, "Payment server: another instance is answering on the URI endpoint; not taking it over\n");
            return;
        }
        // Clean up a socket left by a crashed process, then claim the endpoint.
        QLocalServer::removeServer(name);
        uriServer = new QLocalServer(this);

        if (!uriServer->listen(name)) {
            // constructor is called early in init, so don't use "Q_EMIT message()" here
            auto* box = new QMessageBox(QMessageBox::Critical, tr("Payment request error"),
                tr("Cannot start quicksilver: click-to-pay handler"), QMessageBox::Ok, nullptr);
            box->setObjectName(QStringLiteral("paymentRequestHandlerError"));
            GUIUtil::ShowModalDialogAsynchronously(box);
        }
        else {
            connect(uriServer, &QLocalServer::newConnection, this, &PaymentServer::handleURIConnection);
        }
    }
}

PaymentServer::~PaymentServer() = default;

//
// OSX-specific way of handling quicksilver: URIs
//
bool PaymentServer::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::FileOpen) {
        QFileOpenEvent *fileEvent = static_cast<QFileOpenEvent*>(event);
        if (!fileEvent->file().isEmpty())
            handleURIOrFile(fileEvent->file());
        else if (!fileEvent->url().isEmpty())
            handleURIOrFile(fileEvent->url().toString());

        return true;
    }

    return QObject::eventFilter(object, event);
}

void PaymentServer::uiReady()
{
    saveURIs = false;
    for (const QString& s : savedPaymentRequests)
    {
        handleURIOrFile(s);
    }
    savedPaymentRequests.clear();
}

void PaymentServer::handleURIOrFile(const QString& s)
{
    if (saveURIs)
    {
        savedPaymentRequests.insert(s);
        return;
    }

    if (s.startsWith("quicksilver://", Qt::CaseInsensitive))
    {
        Q_EMIT message(tr("URI handling"), tr("'quicksilver://' is not a valid URI. Use 'quicksilver:' instead."),
            CClientUIInterface::MSG_ERROR);
    }
    else if (s.startsWith(QUICKSILVER_IPC_PREFIX, Qt::CaseInsensitive)) // quicksilver: URI
    {
        QUrlQuery uri((QUrl(s)));
        // normal URI
        {
            SendCoinsRecipient recipient;
            if (GUIUtil::parseQuicksilverURI(s, &recipient))
            {
                std::string error_msg;
                const CTxDestination dest = DecodeDestination(recipient.address.toStdString(), error_msg);

                if (!IsValidDestination(dest)) {
                    if (uri.hasQueryItem("r")) {  // payment request
                        Q_EMIT message(tr("URI handling"),
                            tr("This kind of payment request is not supported.\n"
                               "Ask for a quicksilver: URI that names the address directly."),
                            CClientUIInterface::ICON_WARNING);
                    }
                    Q_EMIT message(tr("URI handling"), QString::fromStdString(error_msg),
                        CClientUIInterface::MSG_ERROR);
                }
                else
                    Q_EMIT receivedPaymentRequest(recipient);
            }
            else
                Q_EMIT message(tr("URI handling"),
                    tr("URI cannot be parsed! This can be caused by an invalid Quicksilver address or malformed URI parameters."),
                    CClientUIInterface::ICON_WARNING);

            return;
        }
    }

    if (QFile::exists(s)) // payment request file
    {
        Q_EMIT message(tr("Payment request file handling"),
            tr("This kind of payment request is not supported.\n"
               "Ask for a quicksilver: URI that names the address directly."),
            CClientUIInterface::ICON_WARNING);
    }
}

void PaymentServer::handleURIConnection()
{
    while (uriServer->hasPendingConnections()) {
        auto* client = uriServer->nextPendingConnection();
        auto bytes = std::make_shared<QByteArray>();
        auto done = std::make_shared<bool>(false);
        auto* timer = new QTimer(client);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, client, [client, done] {
            if (*done) return;
            *done = true;
            LogInfo(HgLog::QT, "Payment server: dropped a URI connection (%s)\n", "timed out");
            client->abort();
            client->deleteLater();
        });
        const auto read_frame = [this, client, bytes, done, timer] {
            if (*done) return;
            bytes->append(client->readAll());
            QString uri;
            const char* reason = nullptr;
            const auto status = PaymentServerFrame::decode(*bytes, &uri, &reason);
            if (status == PaymentServerFrame::Status::Incomplete) return;
            *done = true;
            timer->stop();
            if (status == PaymentServerFrame::Status::Complete) {
                handleURIOrFile(uri);
            } else {
                LogInfo(HgLog::QT, "Payment server: dropped a URI connection (%s)\n", reason);
            }
            client->disconnectFromServer();
            client->deleteLater();
        };
        connect(client, &QLocalSocket::readyRead, client, read_frame);
        // Qt does not promise readyRead before disconnected: read what is buffered first.
        connect(client, &QLocalSocket::disconnected, client, [client, done, read_frame] {
            read_frame();
            if (!*done) {
                *done = true;
                LogInfo(HgLog::QT, "Payment server: dropped a URI connection (%s)\n", "closed early");
            }
            client->deleteLater();
        });
        timer->start(QUICKSILVER_IPC_CONNECT_TIMEOUT);
        read_frame();
    }
}

void PaymentServer::setOptionsModel(OptionsModel *_optionsModel)
{
    this->optionsModel = _optionsModel;
}
