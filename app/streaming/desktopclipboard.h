#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QUrl>

class QNetworkReply;

// Optional clipboard exchange over the paired host's TLS service. All native clipboard
// access stays on the GUI thread; requests are asynchronous and bounded.
class DesktopClipboard : public QObject
{
public:
    DesktopClipboard(QUrl host, QSslCertificate certificate, QSslConfiguration identity);
    ~DesktopClipboard() override;
    void pump();

private:
    friend class DesktopClipboardTest;
    QJsonObject snapshot(bool& blocked) const;
    QJsonObject captureSnapshot(bool& blocked) const;
    bool apply(const QJsonObject& items);
    static QByteArray digest(const QJsonObject& items);
    void exchange(const QJsonObject& local, bool blocked);

    QNetworkAccessManager m_Network;
    QPointer<QNetworkReply> m_Reply;
    QUrl m_Url;
    QSslCertificate m_ServerCert;
    QSslConfiguration m_Identity;
    QElapsedTimer m_LastPoll;
    QByteArray m_Local;
    QByteArray m_SyncedLocal;
    QByteArray m_Remote;
    bool m_Initialized = false;
    bool m_Disabled = false;
    quint64 m_ClipboardRevision = 0;
    mutable quint64 m_CachedRevision = UINT64_MAX;
    mutable QJsonObject m_CachedSnapshot;
    mutable bool m_CachedBlocked = false;
};
