#include "desktopclipboard.h"

#include <QBuffer>
#include <QClipboard>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QMimeData>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QTimer>

namespace {
constexpr int MaxBytes = 4 * 1024 * 1024;
constexpr int MaxWireBytes = 6 * 1024 * 1024;
const QStringList Formats = {"text/plain", "text/html", "text/rtf", "image/png"};
}

DesktopClipboard::DesktopClipboard(QUrl host, QSslCertificate certificate, QSslConfiguration identity)
    : m_Url(std::move(host)), m_ServerCert(std::move(certificate)), m_Identity(std::move(identity))
{
    m_Url.setPath("/arf/clipboard");
    m_Url.setQuery(QString());
    m_Disabled = m_ServerCert.isNull();
    m_LastPoll.start();
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, [this]() { m_ClipboardRevision++; });
}

DesktopClipboard::~DesktopClipboard()
{
    if (m_Reply) {
        disconnect(m_Reply, nullptr, this, nullptr);
        m_Reply->abort();
    }
}

QByteArray DesktopClipboard::digest(const QJsonObject& items)
{
    return QCryptographicHash::hash(QJsonDocument(items).toJson(QJsonDocument::Compact),
                                     QCryptographicHash::Sha256).toHex();
}

QJsonObject DesktopClipboard::snapshot(bool& blocked) const
{
    if (m_CachedRevision != m_ClipboardRevision) {
        const auto revision = m_ClipboardRevision;
        m_CachedSnapshot = captureSnapshot(m_CachedBlocked);
        m_CachedRevision = revision;
    }
    blocked = m_CachedBlocked;
    return m_CachedSnapshot;
}

QJsonObject DesktopClipboard::captureSnapshot(bool& blocked) const
{
    blocked = false;
    QJsonObject result;
    const auto* mime = QGuiApplication::clipboard()->mimeData();
    if (!mime) return result;
    for (const auto& marker : {"org.nspasteboard.ConcealedType", "org.nspasteboard.TransientType",
                              "application/x-keepassxc", "x-kde-passwordManagerHint"}) {
        if (mime->hasFormat(marker)) { blocked = true; return {}; }
    }
    int total = 0;
    for (const auto& format : Formats) {
        QByteArray data;
        if (format == "text/plain" && mime->hasText()) data = mime->text().toUtf8();
        else if (mime->hasFormat(format)) data = mime->data(format);
        else if (format == "image/png" && mime->hasImage()) {
            QBuffer buffer(&data);
            buffer.open(QIODevice::WriteOnly);
            const auto image = qvariant_cast<QImage>(mime->imageData());
            if (image.isNull() || image.width() > 16384 || image.height() > 16384 ||
                qint64(image.width()) * image.height() > 64000000 || !image.save(&buffer, "PNG")) {
                blocked = true; return {};
            }
        }
        else continue;
        total += data.size();
        if (total > MaxBytes) { blocked = true; return {}; }
        result[format] = QString::fromLatin1(data.toBase64());
    }
    // Never translate a file selection or an unsupported clipboard into a remote clear.
    if (mime->hasUrls() || (result.isEmpty() && !mime->formats().isEmpty())) blocked = true;
    return result;
}

bool DesktopClipboard::apply(const QJsonObject& items)
{
    if (items.size() > Formats.size()) return false;
    auto mime = std::make_unique<QMimeData>();
    int total = 0;
    for (auto i = items.begin(); i != items.end(); ++i) {
        if (!Formats.contains(i.key()) || !i.value().isString()) return false;
        const auto wire = i.value().toString().toLatin1();
        const auto data = QByteArray::fromBase64(wire);
        if (data.toBase64() != wire) return false;
        total += data.size();
        if (total > MaxBytes) return false;
        if (i.key() == "text/plain" && QString::fromUtf8(data).toUtf8() != data) return false;
        if (i.key() == "image/png") {
            // Check dimensions before invoking an image decoder.
            if (data.size() < 24 || data.left(8) != QByteArray::fromHex("89504e470d0a1a0a")) return false;
            const auto dim = [&data](int offset) {
                return (quint32(quint8(data[offset])) << 24) | (quint32(quint8(data[offset + 1])) << 16) |
                       (quint32(quint8(data[offset + 2])) << 8) | quint8(data[offset + 3]);
            };
            const auto w = dim(16), h = dim(20);
            if (!w || !h || w > 16384 || h > 16384 || quint64(w) * h > 64000000) return false;
            const auto image = QImage::fromData(data, "PNG");
            if (image.isNull()) return false;
            mime->setImageData(image);
        }
        mime->setData(i.key(), data);
    }
    QGuiApplication::clipboard()->setMimeData(mime.release());
    return true;
}

void DesktopClipboard::pump()
{
    if (m_Disabled || m_LastPoll.elapsed() < 250) return;
    m_LastPoll.restart();
    // SDL owns the streaming event loop. Pump only non-input Qt events so clipboard
    // ownership and the asynchronous TLS request can progress without activating QML controls.
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 2);
    bool blocked;
    const auto local = snapshot(blocked);
    m_Local = digest(local);
    if (!m_Initialized) {
        m_SyncedLocal = m_Local;
        m_Initialized = true;
    }
    if (!m_Reply) exchange(local, blocked);
}

void DesktopClipboard::exchange(const QJsonObject& local, bool blocked)
{
    QJsonObject message {{"version", 1}};
    if (!m_Remote.isEmpty()) message["known"] = QString::fromLatin1(m_Remote);
    const auto requestLocal = m_Local;
    const bool writing = !blocked && !m_Remote.isEmpty() && m_Local != m_SyncedLocal;
    if (writing) {
        message["expected"] = QString::fromLatin1(m_Remote);
        message["write"] = local;
    }
    QNetworkRequest request(m_Url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setSslConfiguration(m_Identity);
    m_Reply = m_Network.post(request, QJsonDocument(message).toJson(QJsonDocument::Compact));
    auto* reply = m_Reply.data();
    QTimer::singleShot(2000, reply, [reply]() { if (!reply->isFinished()) reply->abort(); });
    connect(reply, &QNetworkReply::sslErrors, this, [this, reply](const QList<QSslError>& errors) {
        for (const auto& error : errors) if (error.certificate() != m_ServerCert) return;
        reply->ignoreSslErrors(errors);
    });
    connect(reply, &QNetworkReply::encrypted, this, [this, reply]() {
        if (reply->sslConfiguration().peerCertificate() != m_ServerCert) reply->abort();
    });
    connect(reply, &QNetworkReply::readyRead, this, [reply]() {
        if (reply->bytesAvailable() > MaxWireBytes) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, requestLocal, writing]() {
        m_Reply = nullptr;
        reply->deleteLater();
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 403 || status == 404) {
            qInfo() << "Clipboard sharing unavailable: enable it on a compatible host with an active stream";
            m_Disabled = true;
            return;
        }
        if ((status != 200 && status != 409) || reply->bytesAvailable() > MaxWireBytes ||
            reply->sslConfiguration().peerCertificate() != m_ServerCert) return;
        const auto body = reply->readAll();
        const auto response = QJsonDocument::fromJson(body).object();
        if (response.value("version").toInt() != 1 || !response.value("blocked").isBool() ||
            response.value("blocked").toBool()) return;
        if (response.value("unchanged").toBool() && response.value("digest").toString().toLatin1() == m_Remote) {
            if (writing && status == 200) m_SyncedLocal = requestLocal;
            return;
        }
        if (!response.value("items").isObject()) return;
        const auto items = response.value("items").toObject();
        const auto remote = response.value("digest").toString().toLatin1();
        if (digest(items) != remote) return;
        bool nowBlocked;
        const auto now = snapshot(nowBlocked);
        const auto nowDigest = digest(now);
        const bool unchanged = !nowBlocked && nowDigest == requestLocal;
        // Establish a baseline without replacing either machine's pre-existing clipboard.
        const bool remoteChanged = !m_Remote.isEmpty() && remote != m_Remote;
        const bool shouldApply = remoteChanged && unchanged &&
                (status == 409 || (!writing && nowDigest == m_SyncedLocal) ||
                 (writing && status == 200 && remote != requestLocal));
        if (writing && status == 200) m_SyncedLocal = requestLocal;
        if (shouldApply) {
            if (apply(items)) {
                bool ignored;
                m_Local = digest(snapshot(ignored));
                m_SyncedLocal = m_Local;
            }
        }
        m_Remote = remote;
    });
}
