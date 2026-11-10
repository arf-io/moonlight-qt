#include "../../app/streaming/desktopclipboard.h"
#include "../../app/streaming/mediaclock.h"

#include <QBuffer>
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QtTest>
#include <QProcess>
#include <QTemporaryDir>
#include "tlshost.h"

class DesktopClipboardTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(m_Keys.isValid());
        const auto result = QProcess::execute("openssl", {"req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1", "-days", "1",
            "-keyout", m_Keys.filePath("key.pem"), "-out", m_Keys.filePath("cert.pem")});
        QCOMPARE(result, 0);
        QCOMPARE(QProcess::execute("openssl", {"req", "-x509", "-new", "-key", m_Keys.filePath("key.pem"),
            "-subj", "/CN=wrong-host", "-days", "1", "-out", m_Keys.filePath("other.pem")}), 0);
    }
    void init() { QGuiApplication::clipboard()->clear(); }

    void unicodeAndRichTextRoundTrip()
    {
        DesktopClipboard bridge(QUrl("https://127.0.0.1:1"), {}, {});
        const auto text = QString::fromUtf8("Command ⌘ — hello 🌍\nsecond line").toUtf8();
        const QJsonObject items {{"text/plain", QString::fromLatin1(text.toBase64())},
                                 {"text/html", QString::fromLatin1(QByteArray("<b>hello</b>").toBase64())}};
        QVERIFY(bridge.apply(items));
        bool blocked;
        QCOMPARE(bridge.snapshot(blocked), items);
        QVERIFY(!blocked);
        QCOMPARE(QGuiApplication::clipboard()->text().toUtf8(), text);
    }

    void imageRoundTrip()
    {
        DesktopClipboard bridge(QUrl("https://127.0.0.1:1"), {}, {});
        QImage image(3, 2, QImage::Format_ARGB32);
        image.fill(Qt::red);
        QByteArray data;
        QBuffer buffer(&data);
        buffer.open(QIODevice::WriteOnly);
        QVERIFY(image.save(&buffer, "PNG"));
        QVERIFY(bridge.apply({{"image/png", QString::fromLatin1(data.toBase64())}}));
        QCOMPARE(QGuiApplication::clipboard()->image().size(), image.size());
        bool blocked;
        QVERIFY(bridge.snapshot(blocked).contains("image/png"));
        QVERIFY(!blocked);
    }

    void rejectsMalformedOversizedOrUnsupportedData()
    {
        DesktopClipboard bridge(QUrl("https://127.0.0.1:1"), {}, {});
        QGuiApplication::clipboard()->setText("preserve");
        QVERIFY(!bridge.apply({{"text/plain", 1}}));
        QVERIFY(!bridge.apply({{"text/plain", "YR=="}}));
        QVERIFY(!bridge.apply({{"text/plain", "/w=="}}));
        QVERIFY(!bridge.apply({{"image/png", "YQ=="}}));
        QVERIFY(!bridge.apply({{"text/uri-list", "YQ=="}}));
        QVERIFY(!bridge.apply({{"text/plain", QString(6 * 1024 * 1024, 'A')}}));
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("preserve"));
    }

    void doesNotShareConcealedOrFileClipboards()
    {
        DesktopClipboard bridge(QUrl("https://127.0.0.1:1"), {}, {});
        auto* concealed = new QMimeData();
        concealed->setText("concealed fixture");
        concealed->setData("org.nspasteboard.ConcealedType", "1");
        QGuiApplication::clipboard()->setMimeData(concealed);
        bool blocked;
        QVERIFY(bridge.snapshot(blocked).isEmpty());
        QVERIFY(blocked);
        auto* files = new QMimeData();
        files->setUrls({QUrl::fromLocalFile("/tmp/example")});
        QGuiApplication::clipboard()->setMimeData(files);
        bridge.snapshot(blocked);
        QVERIFY(blocked);
    }

    void clearAndDigestAreProtocolCompatible()
    {
        DesktopClipboard bridge(QUrl("https://127.0.0.1:1"), {}, {});
        QCOMPARE(DesktopClipboard::digest({}), QByteArray("44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a"));
        QGuiApplication::clipboard()->setText("before");
        QVERIFY(bridge.apply({}));
        bool blocked;
        QVERIFY(bridge.snapshot(blocked).isEmpty());
        QVERIFY(!blocked);
        QVERIFY(bridge.m_Disabled); // A missing host pin must fail closed.
    }

    void pairedTlsSharingDoesNotEchoOrOverwriteInitialClipboard()
    {
        ClipboardTlsHost host;
        configure(host);
        QVERIFY(host.listen(QHostAddress::LocalHost, 0));
        host.items = {{"text/plain", "aG9zdCBiZWZvcmU="}};
        QGuiApplication::clipboard()->setText("client before");
        DesktopClipboard bridge(QUrl(QString("https://127.0.0.1:%1").arg(host.serverPort())), host.certificate, identity(host));
        QVERIFY(pumpUntil(bridge, [&]() { return !bridge.m_Remote.isEmpty(); }));
        QVERIFY(host.sawClientCertificate);
        QCOMPARE(host.writes, 0);
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("client before"));
        QGuiApplication::clipboard()->setText("client copy");
        QVERIFY(pumpUntil(bridge, [&]() { return host.writes == 1; }));
        QCOMPARE(host.items.value("text/plain").toString(), QString("Y2xpZW50IGNvcHk="));
        QVERIFY(pumpUntil(bridge, [&]() { return bridge.m_Reply.isNull(); }));
        host.items = {{"text/plain", "aG9zdCBjb3B5"}};
        QVERIFY(pumpUntil(bridge, [&]() { return QGuiApplication::clipboard()->text() == "host copy"; }));
        const auto requests = host.requests;
        QVERIFY(pumpUntil(bridge, [&]() { return host.requests >= requests + 2; }));
        QCOMPARE(host.writes, 1);
    }

    void staleReplyCannotReplaceNewLocalCopy()
    {
        ClipboardTlsHost host;
        configure(host);
        QVERIFY(host.listen(QHostAddress::LocalHost, 0));
        QGuiApplication::clipboard()->setText("baseline");
        DesktopClipboard bridge(QUrl(QString("https://127.0.0.1:%1").arg(host.serverPort())), host.certificate, identity(host));
        QVERIFY(pumpUntil(bridge, [&]() { return !bridge.m_Remote.isEmpty(); }));
        host.pause = true;
        QVERIFY(pumpUntil(bridge, [&]() { return bool(host.releaseResponse); }));
        host.items = {{"text/plain", "b2xkIHJlbW90ZQ=="}};
        QGuiApplication::clipboard()->setText("new local");
        host.pause = false;
        host.releaseResponse();
        host.releaseResponse = {};
        QVERIFY(pumpUntil(bridge, [&]() { return host.writes > 0; }));
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("new local"));
        QCOMPARE(host.items.value("text/plain").toString(), QString("bmV3IGxvY2Fs"));
    }

    void unavailableHostStopsPolling()
    {
        ClipboardTlsHost host;
        configure(host);
        host.status = 403;
        QVERIFY(host.listen(QHostAddress::LocalHost, 0));
        DesktopClipboard bridge(QUrl(QString("https://127.0.0.1:%1").arg(host.serverPort())), host.certificate, identity(host));
        QVERIFY(pumpUntil(bridge, [&]() { return bridge.m_Disabled; }));
        QCOMPARE(host.writes, 0);
    }

    void rejectsDifferentHostCertificate()
    {
        ClipboardTlsHost host;
        configure(host);
        QVERIFY(host.listen(QHostAddress::LocalHost, 0));
        QFile wrong(m_Keys.filePath("other.pem"));
        QVERIFY(wrong.open(QIODevice::ReadOnly));
        DesktopClipboard bridge(QUrl(QString("https://127.0.0.1:%1").arg(host.serverPort())),
                                QSslCertificate(wrong.readAll()), identity(host));
        QVERIFY(pumpUntil(bridge, [&]() { return bridge.m_Initialized && bridge.m_Reply.isNull(); }));
        QCOMPARE(host.requests, 0);
        QVERIFY(bridge.m_Remote.isEmpty());
    }

    void concurrentHostCopyWinsAWriteConflictWithoutEcho()
    {
        ClipboardTlsHost host;
        configure(host);
        QVERIFY(host.listen(QHostAddress::LocalHost, 0));
        QGuiApplication::clipboard()->setText("baseline");
        DesktopClipboard bridge(QUrl(QString("https://127.0.0.1:%1").arg(host.serverPort())), host.certificate, identity(host));
        QVERIFY(pumpUntil(bridge, [&]() { return !bridge.m_Remote.isEmpty(); }));
        host.pause = true;
        QGuiApplication::clipboard()->setText("concurrent client");
        QVERIFY(pumpUntil(bridge, [&]() { return bool(host.releaseResponse); }));
        host.items = {{"text/plain", "Y29uY3VycmVudCBob3N0"}};
        host.pause = false;
        host.releaseResponse();
        host.releaseResponse = {};
        QVERIFY(pumpUntil(bridge, [&]() { return QGuiApplication::clipboard()->text() == "concurrent host"; }));
        QVERIFY(host.sawWriteConflict);
        QCOMPARE(host.writes, 0);
        const auto requests = host.requests;
        QVERIFY(pumpUntil(bridge, [&]() { return host.requests > requests; }));
        QCOMPARE(host.writes, 0);
    }

    void mediaClockHandlesIndependentEpochsAndRtpWrap()
    {
        auto& clock = MediaClock::get();
        clock.reset(true);
        const uint64_t host = (uint64_t(UINT32_MAX) * 1000 / 90) - 5000;
        const uint64_t local = 7000000;
        clock.update(host, local + 20000, local);
        const auto video = uint32_t((host + 10000) * 90 / 1000);
        const auto delay = clock.videoDelay(video, local);
        QVERIFY(delay);
        QVERIFY(std::abs(*delay - 30000) < 25);
        QVERIFY(!clock.videoDelay(video, local + 1000001));
        clock.reset(false);
        clock.update(host, local, local);
        QVERIFY(!clock.videoDelay(video, local));
    }

    void unchangedClipboardDoesNotConsumeStreamingBandwidth()
    {
        ClipboardTlsHost host;
        configure(host);
        QVERIFY(host.listen(QHostAddress::LocalHost, 0));
        host.items = {{"text/plain", QString(400000, 'A')}};
        DesktopClipboard bridge(QUrl(QString("https://127.0.0.1:%1").arg(host.serverPort())), host.certificate, identity(host));
        QVERIFY(pumpUntil(bridge, [&]() { return !bridge.m_Remote.isEmpty(); }));
        const auto bytes = host.responseBytes, requests = host.requests;
        QVERIFY(pumpUntil(bridge, [&]() { return host.requests >= requests + 2; }));
        QVERIFY(host.responseBytes - bytes < 1024);
        QCOMPARE(host.writes, 0);
    }

    void mediaClockTracksThirtyMinutesOfDeviceDrift()
    {
        auto& clock = MediaClock::get();
        clock.reset(true);
        const uint64_t hostEpoch = 987654321000, localEpoch = 123450000;
        for (uint64_t elapsed = 0; elapsed <= 1800000000; elapsed += 5000) {
            const auto host = hostEpoch + elapsed;
            const auto now = localEpoch + elapsed + elapsed / 10000; // 100 ppm clock drift
            clock.update(host, now + 20000, now);
            const auto video = uint32_t((host + 10000) * 90 / 1000);
            const auto delay = clock.videoDelay(video, now);
            QVERIFY(delay);
            QVERIFY(std::abs(*delay - 30000) < 250);
        }
        clock.reset(false);
    }

private:
    QTemporaryDir m_Keys;

    void configure(ClipboardTlsHost& host)
    {
        QFile cert(m_Keys.filePath("cert.pem")), key(m_Keys.filePath("key.pem"));
        QVERIFY(cert.open(QIODevice::ReadOnly));
        QVERIFY(key.open(QIODevice::ReadOnly));
        host.certificate = QSslCertificate(cert.readAll());
        host.key = QSslKey(key.readAll(), QSsl::Rsa);
    }

    QSslConfiguration identity(const ClipboardTlsHost& host)
    {
        auto config = QSslConfiguration::defaultConfiguration();
        config.setLocalCertificate(host.certificate);
        config.setPrivateKey(host.key);
        return config;
    }

    bool pumpUntil(DesktopClipboard& bridge, const std::function<bool()>& complete)
    {
        QElapsedTimer deadline;
        deadline.start();
        while (!complete() && deadline.elapsed() < 5000) {
            bridge.pump();
            QTest::qWait(10);
        }
        return complete();
    }
};

QTEST_MAIN(DesktopClipboardTest)
#include "test_desktopclipboard.moc"
