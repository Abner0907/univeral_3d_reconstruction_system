#include "ApiService.h"

#include "JobManager.h"
#include "Pipeline.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkInterface>

namespace u3d {

ApiService::ApiService(JobManager *jobs, QObject *parent)
    : QObject(parent)
    , m_jobs(jobs)
{
    m_server.setHandler([this](const HttpRequest &r) { return handle(r); });
}

bool ApiService::start(quint16 port, QString *error)
{
    if (m_server.isListening())
        m_server.close();
    if (!m_server.listen(QHostAddress::Any, port)) {
        if (error)
            *error = m_server.errorString();
        return false;
    }
    return true;
}

void ApiService::stop() { m_server.close(); }

QStringList ApiService::lanUrls() const
{
    QStringList urls;
    const auto addresses = QNetworkInterface::allAddresses();
    for (const QHostAddress &addr : addresses) {
        if (addr.protocol() == QAbstractSocket::IPv4Protocol && !addr.isLoopback())
            urls << QStringLiteral("http://%1:%2").arg(addr.toString()).arg(port());
    }
    if (urls.isEmpty())
        urls << QStringLiteral("http://127.0.0.1:%1").arg(port());
    return urls;
}

HttpResponse ApiService::handle(const HttpRequest &req)
{
    const QStringList parts = req.path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    const QByteArray &method = req.method;
    auto notFound = [] { return HttpResponse::error(404, QStringLiteral("Not found")); };
    auto notAllowed = [] { return HttpResponse::error(405, QStringLiteral("Method not allowed")); };

    if (parts.size() < 2 || parts[0] != QLatin1String("api"))
        return notFound();

    // /api/health, /api/info
    if (parts.size() == 2 && parts[1] == QLatin1String("health"))
        return HttpResponse::json(QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")}});
    if (parts.size() == 2 && parts[1] == QLatin1String("info")) {
        const Settings s = m_jobs->settings();
        return HttpResponse::json(QJsonObject{
            {QStringLiteral("name"), QStringLiteral("Universal 3D Reconstruction System")},
            {QStringLiteral("version"), QStringLiteral(U3D_VERSION)},
            {QStringLiteral("dry_run"), s.dryRun},
            {QStringLiteral("methods"), QJsonArray::fromStringList(pipeline::methods())},
            {QStringLiteral("default_method"), s.defaultMethod},
            {QStringLiteral("default_iterations"), s.defaultIterations},
            {QStringLiteral("viewer_port"), s.viewerPort},
        });
    }
    if (parts[1] != QLatin1String("jobs"))
        return notFound();

    // /api/jobs
    if (parts.size() == 2) {
        if (method == "GET") {
            QJsonArray arr;
            for (const Job &job : m_jobs->jobs())
                arr.append(job.toJson());
            return HttpResponse::json(QJsonObject{{QStringLiteral("jobs"), arr}});
        }
        if (method == "POST") {
            QJsonObject body;
            if (!req.body.trimmed().isEmpty()) {
                const QJsonDocument doc = QJsonDocument::fromJson(req.body);
                if (!doc.isObject())
                    return HttpResponse::error(400, QStringLiteral("Body must be a JSON object"));
                body = doc.object();
            }
            QString error;
            const QString id = m_jobs->createJob(body.value(QStringLiteral("name")).toString(),
                                                 body.value(QStringLiteral("method")).toString(),
                                                 body.value(QStringLiteral("iterations")).toInt(0),
                                                 &error);
            if (id.isEmpty())
                return HttpResponse::error(400, error);
            return HttpResponse::json(m_jobs->job(id).toJson(), 201);
        }
        return notAllowed();
    }

    const QString id = parts[2];
    if (!m_jobs->hasJob(id))
        return HttpResponse::error(404, QStringLiteral("Job not found"));

    // /api/jobs/{id}
    if (parts.size() == 3) {
        if (method == "GET")
            return HttpResponse::json(m_jobs->job(id).toJson());
        if (method == "DELETE") {
            m_jobs->removeJob(id);
            return HttpResponse::noContent();
        }
        return notAllowed();
    }

    const QString action = parts[3];
    // /api/jobs/{id}/files/{name}
    if (action == QLatin1String("files") && parts.size() == 5) {
        if (method != "PUT" && method != "POST")
            return notAllowed();
        if (req.body.isEmpty())
            return HttpResponse::error(400, QStringLiteral("Empty file"));
        QString error;
        const QString stored = m_jobs->addInput(id, parts[4], req.body, &error);
        if (stored.isEmpty())
            return HttpResponse::error(m_jobs->job(id).status == JobStatus::Uploading ? 400 : 409, error);
        return HttpResponse::json(QJsonObject{{QStringLiteral("file"), stored},
                                              {QStringLiteral("size"), double(req.body.size())}},
                                  201);
    }
    if (parts.size() != 4)
        return notFound();

    if (action == QLatin1String("start")) {
        if (method != "POST")
            return notAllowed();
        QString error;
        if (!m_jobs->startJob(id, &error))
            return HttpResponse::error(400, error);
        return HttpResponse::json(m_jobs->job(id).toJson());
    }
    if (action == QLatin1String("cancel")) {
        if (method != "POST")
            return notAllowed();
        m_jobs->cancelJob(id);
        return HttpResponse::json(m_jobs->job(id).toJson());
    }
    if (action == QLatin1String("log")) {
        if (method != "GET")
            return notAllowed();
        bool ok = false;
        qint64 tail = req.query.queryItemValue(QStringLiteral("tail")).toLongLong(&ok);
        if (!ok || tail <= 0)
            tail = 64 * 1024;
        return HttpResponse::text(m_jobs->logTail(id, qMin<qint64>(tail, 4 * 1024 * 1024)));
    }
    if (action == QLatin1String("result")) {
        if (method != "GET")
            return notAllowed();
        const QString path = m_jobs->resultPath(id);
        if (path.isEmpty())
            return HttpResponse::error(404, QStringLiteral("No result available"));
        const Job job = m_jobs->job(id);
        const QString name = pipeline::sanitizeFileName(job.name, job.id) + QLatin1Char('_')
                             + job.method + QStringLiteral(".ply");
        return HttpResponse::file(path, name);
    }
    return notFound();
}

} // namespace u3d
