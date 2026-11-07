// End-to-end test of the REST API the Android app uses, over real HTTP.

#include "ApiService.h"
#include "JobManager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QTcpSocket>
#include <QtTest>

using namespace u3d;

class TestApiService : public QObject
{
    Q_OBJECT

private:
    struct Reply {
        int status = 0;
        QByteArray body;
        QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
    };

    QTemporaryDir m_data;
    std::unique_ptr<JobManager> m_jobs;
    std::unique_ptr<ApiService> m_api;
    QNetworkAccessManager m_nam;

    QUrl url(const QString &path) const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_api->port()).arg(path));
    }

    Reply call(const QByteArray &verb, const QString &path, const QByteArray &body = {},
               const QByteArray &contentType = "application/json")
    {
        QNetworkRequest req(url(path));
        if (!body.isEmpty())
            req.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
        QNetworkReply *reply = m_nam.sendCustomRequest(req, verb, body);
        QSignalSpy done(reply, &QNetworkReply::finished);
        if (!reply->isFinished())
            done.wait(10000);
        Reply r;
        r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        r.body = reply->readAll();
        reply->deleteLater();
        return r;
    }

private slots:
    void initTestCase()
    {
        qputenv("U3D_FAKE_STEP_DELAY", "0.01");
        Settings s;
        s.dataDir = m_data.path();
        s.dryRun = true;
        s.fakeNsPath = QStringLiteral(U3D_FAKE_NS_PATH);
        m_jobs = std::make_unique<JobManager>(s);
        m_api = std::make_unique<ApiService>(m_jobs.get());
        QString err;
        QVERIFY2(m_api->start(0, &err), qPrintable(err)); // any free port
        QVERIFY(m_api->port() > 0);
        m_nam.setProxy(QNetworkProxy::NoProxy); // ignore http_proxy on CI machines
    }

    void cleanupTestCase()
    {
        m_api.reset();
        m_jobs.reset();
    }

    void info()
    {
        const Reply r = call("GET", "/api/info");
        QCOMPARE(r.status, 200);
        QCOMPARE(r.json().value("dry_run").toBool(), true);
        QVERIFY(r.json().value("methods").toArray().contains(QStringLiteral("splatfacto")));
        QCOMPARE(call("GET", "/api/health").status, 200);
        QCOMPARE(call("GET", "/nope").status, 404);
    }

    void fullCaptureFlow()
    {
        // 1. create
        Reply r = call("POST", "/api/jobs", R"({"name":"Phone scan","method":"splatfacto","iterations":2000})");
        QCOMPARE(r.status, 201);
        const QString id = r.json().value("id").toString();
        QCOMPARE(r.json().value("status").toString(), QStringLiteral("uploading"));

        // 2. upload images one by one (as the Android app does)
        for (int i = 0; i < 4; ++i) {
            r = call("PUT", QStringLiteral("/api/jobs/%1/files/IMG_%2.jpg").arg(id).arg(i),
                     QByteArray(50000 + i, char(i)), "image/jpeg");
            QCOMPARE(r.status, 201);
            QCOMPARE(r.json().value("size").toInt(), 50000 + i);
        }
        QCOMPARE(QDir(m_jobs->jobDir(id) + "/input").entryList(QDir::Files).size(), 4);

        // 3. start and follow progress
        r = call("POST", QStringLiteral("/api/jobs/%1/start").arg(id));
        QCOMPARE(r.status, 200);
        QCOMPARE(r.json().value("num_inputs").toInt(), 4);
        QTRY_COMPARE_WITH_TIMEOUT(call("GET", "/api/jobs/" + id).json().value("status").toString(),
                                  QStringLiteral("succeeded"), 30000);

        r = call("GET", "/api/jobs");
        QCOMPARE(r.json().value("jobs").toArray().first().toObject().value("id").toString(), id);

        // 4. log + result download
        r = call("GET", QStringLiteral("/api/jobs/%1/log?tail=4096").arg(id));
        QCOMPARE(r.status, 200);
        QVERIFY(r.body.contains("Reconstruction complete"));
        r = call("GET", QStringLiteral("/api/jobs/%1/result").arg(id));
        QCOMPARE(r.status, 200);
        QVERIFY(r.body.startsWith("ply\n"));
        QCOMPARE(qint64(r.body.size()), QFileInfo(m_jobs->resultPath(id)).size());

        // 5. uploads after start are rejected; delete works
        QCOMPARE(call("PUT", QStringLiteral("/api/jobs/%1/files/late.jpg").arg(id), "x", "image/jpeg").status, 409);
        QCOMPARE(call("DELETE", "/api/jobs/" + id).status, 204);
        QCOMPARE(call("GET", "/api/jobs/" + id).status, 404);
    }

    void validationErrors()
    {
        QCOMPARE(call("POST", "/api/jobs", R"({"method":"bogus"})").status, 400);
        QCOMPARE(call("POST", "/api/jobs", "not json").status, 400);
        const QString id = call("POST", "/api/jobs", "{}").json().value("id").toString();
        QVERIFY(!id.isEmpty());
        QCOMPARE(call("PUT", QStringLiteral("/api/jobs/%1/files/virus.exe").arg(id), "MZ", "application/octet-stream").status, 400);
        Reply r = call("POST", QStringLiteral("/api/jobs/%1/start").arg(id));
        QCOMPARE(r.status, 400);
        QVERIFY(r.json().value("error").toString().contains("No input"));
        QCOMPARE(call("POST", QStringLiteral("/api/jobs/%1/cancel").arg(id)).json().value("status").toString(),
                 QStringLiteral("cancelled"));
        QCOMPARE(call("GET", "/api/jobs/doesnotexist").status, 404);
        QCOMPARE(call("PATCH", "/api/jobs").status, 405);
    }

    void rawSocketKeepAliveAndChunkedRejection()
    {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, m_api->port());
        QVERIFY(socket.waitForConnected(5000));
        // Two pipelined requests on one connection.
        socket.write("GET /api/health HTTP/1.1\r\nHost: x\r\n\r\nGET /api/info HTTP/1.1\r\nHost: x\r\n\r\n");
        QByteArray received;
        QTRY_VERIFY_WITH_TIMEOUT((received += socket.readAll()).count("HTTP/1.1 200 OK") == 2, 5000);
        QVERIFY(received.contains("\"status\":\"ok\""));
        QVERIFY(received.contains("\"dry_run\":true"));

        socket.write("POST /api/jobs HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
        received.clear();
        QTRY_VERIFY_WITH_TIMEOUT((received += socket.readAll()).contains("411 Length Required"), 5000);
    }
};

QTEST_GUILESS_MAIN(TestApiService)
#include "tst_apiservice.moc"
