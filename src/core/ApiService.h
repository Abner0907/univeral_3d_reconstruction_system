#pragma once

#include "HttpServer.h"

#include <QObject>
#include <QStringList>

namespace u3d {

class JobManager;

// REST API over a JobManager. The Android capture app (and scripts) use it to upload images and
// follow progress:
//
//   GET    /api/health
//   GET    /api/info                       server version, methods, defaults
//   GET    /api/jobs                       {"jobs": [...]} newest first
//   POST   /api/jobs                       {"name","method","iterations"} -> job (status "uploading")
//   PUT    /api/jobs/{id}/files/{name}     raw file bytes (one image or one video)
//   POST   /api/jobs/{id}/start            queue the job for reconstruction
//   GET    /api/jobs/{id}                  job status/progress
//   GET    /api/jobs/{id}/log?tail=N       text log
//   POST   /api/jobs/{id}/cancel
//   DELETE /api/jobs/{id}
//   GET    /api/jobs/{id}/result           result .ply
class ApiService : public QObject
{
    Q_OBJECT

public:
    explicit ApiService(JobManager *jobs, QObject *parent = nullptr);

    bool start(quint16 port, QString *error = nullptr);
    void stop();
    bool isRunning() const { return m_server.isListening(); }
    quint16 port() const { return m_server.port(); }

    // Non-loopback IPv4 URLs a phone on the same network can use, e.g. http://192.168.1.20:8000
    QStringList lanUrls() const;

    HttpResponse handle(const HttpRequest &request);

private:
    HttpServer m_server;
    JobManager *m_jobs;
};

} // namespace u3d
