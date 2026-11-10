#pragma once
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>

// An in-process host speaking the actual paired clipboard wire protocol. It uses
// generated test certificates and never listens beyond the loopback interface.
class ClipboardTlsHost : public QTcpServer
{
public:
    QSslCertificate certificate;
    QSslKey key;
    QJsonObject items;
    int requests = 0;
    int writes = 0;
    int responseBytes = 0;
    int status = 200;
    bool sawClientCertificate = false;
    bool sawWriteConflict = false;
    bool pause = false;
    std::function<void()> releaseResponse;

    QByteArray digest() const
    {
        return QCryptographicHash::hash(QJsonDocument(items).toJson(QJsonDocument::Compact),
                                         QCryptographicHash::Sha256).toHex();
    }

protected:
    void incomingConnection(qintptr descriptor) override
    {
        auto* socket = new QSslSocket(this);
        socket->setSocketDescriptor(descriptor);
        auto config = QSslConfiguration::defaultConfiguration();
        config.setLocalCertificate(certificate);
        config.setPrivateKey(key);
        config.setCaCertificates({certificate});
        config.setPeerVerifyMode(QSslSocket::VerifyPeer);
        socket->setSslConfiguration(config);
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QSslSocket::readyRead, socket, [this, socket, buffer]() {
            *buffer += socket->readAll();
            const auto headerEnd = buffer->indexOf("\r\n\r\n");
            if (headerEnd < 0) return;
            int length = 0;
            for (const auto& line : buffer->left(headerEnd).split('\n')) {
                if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
            }
            if (buffer->size() < headerEnd + 4 + length) return;
            disconnect(socket, &QSslSocket::readyRead, socket, nullptr);
            sawClientCertificate = socket->peerCertificate() == certificate;
            const auto message = QJsonDocument::fromJson(buffer->mid(headerEnd + 4, length)).object();
            requests++;
            auto respond = [this, socket, message]() {
                auto responseStatus = status;
                if (message.contains("write") && responseStatus == 200) {
                    if (message.value("expected").toString().toLatin1() != digest()) {
                        responseStatus = 409;
                        sawWriteConflict = true;
                    }
                    else {
                        items = message.value("write").toObject();
                        writes++;
                    }
                }
                QJsonObject response {{"version", 1}, {"blocked", false},
                                            {"items", items}, {"digest", QString::fromLatin1(digest())}};
                if (message.value("known").toString().toLatin1() == digest()) {
                    response.remove("items");
                    response["unchanged"] = true;
                }
                const auto body = QJsonDocument(response).toJson(QJsonDocument::Compact);
                responseBytes += body.size();
                socket->write("HTTP/1.1 " + QByteArray::number(responseStatus) + " Test\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                socket->disconnectFromHost();
            };
            if (pause) releaseResponse = respond;
            else respond();
        });
        socket->startServerEncryption();
    }
};
