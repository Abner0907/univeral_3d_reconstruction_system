// u3d_server - headless reconstruction server for a GPU machine without a desktop session.
// The Android capture app uploads to it; it runs Nerfstudio exactly like the desktop app does.

#include "ApiService.h"
#include "JobManager.h"
#include "Settings.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QTimer>

#include <atomic>
#include <csignal>
#include <cstdio>

namespace {

std::atomic_bool g_stopRequested{false};

void onSignal(int) { g_stopRequested.store(true); }

void say(const QString &text)
{
    std::fprintf(stdout, "%s\n", qPrintable(text));
    std::fflush(stdout);
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Universal3D"));
    QCoreApplication::setApplicationName(QStringLiteral("u3d-server"));
    QCoreApplication::setApplicationVersion(QStringLiteral(U3D_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Universal 3D Reconstruction System - headless server (Nerfstudio + gsplat)"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption portOpt({QStringLiteral("p"), QStringLiteral("port")},
                               QStringLiteral("HTTP port (default 8000)."), QStringLiteral("port"));
    QCommandLineOption dataOpt(QStringLiteral("data-dir"), QStringLiteral("Job storage folder."),
                               QStringLiteral("dir"));
    QCommandLineOption nsOpt(QStringLiteral("ns-bin-dir"),
                             QStringLiteral("Folder with ns-train etc. (default: PATH)."),
                             QStringLiteral("dir"));
    QCommandLineOption visOpt(QStringLiteral("vis"),
                              QStringLiteral("ns-train --vis value (viewer, tensorboard, ...)."),
                              QStringLiteral("vis"));
    QCommandLineOption matchOpt(QStringLiteral("matching-method"),
                                QStringLiteral("COLMAP matching: vocab_tree, exhaustive, sequential."),
                                QStringLiteral("method"));
    QCommandLineOption dryOpt(QStringLiteral("dry-run"),
                              QStringLiteral("Simulate Nerfstudio (no GPU needed)."));
    parser.addOptions({portOpt, dataOpt, nsOpt, visOpt, matchOpt, dryOpt});
    parser.process(app);

    u3d::Settings settings = u3d::Settings::defaults();
    settings.applyEnvironment();
    if (parser.isSet(portOpt))
        settings.serverPort = parser.value(portOpt).toInt();
    if (parser.isSet(dataOpt))
        settings.dataDir = QDir(parser.value(dataOpt)).absolutePath();
    if (parser.isSet(nsOpt))
        settings.nsBinDir = parser.value(nsOpt);
    if (parser.isSet(visOpt))
        settings.vis = parser.value(visOpt);
    if (parser.isSet(matchOpt))
        settings.matchingMethod = parser.value(matchOpt);
    if (parser.isSet(dryOpt))
        settings.dryRun = true;

    u3d::JobManager jobs(settings);
    u3d::ApiService api(&jobs);
    QString error;
    if (!api.start(quint16(settings.serverPort), &error)) {
        std::fprintf(stderr, "Cannot listen on port %d: %s\n", settings.serverPort, qPrintable(error));
        return 1;
    }

    say(QStringLiteral("U3D server %1 - data: %2%3").arg(QStringLiteral(U3D_VERSION), settings.dataDir,
        settings.dryRun ? QStringLiteral(" [DRY RUN: Nerfstudio is simulated]") : QString()));
    for (const QString &url : api.lanUrls())
        say(QStringLiteral("  Listening on %1").arg(url));

    QObject::connect(&jobs, &u3d::JobManager::jobChanged, &app, [&jobs](const QString &id) {
        static QHash<QString, QString> lastState;
        const u3d::Job job = jobs.job(id);
        const QString state = u3d::jobStatusToString(job.status) + QLatin1Char('/') + job.stage;
        if (lastState.value(id) == state)
            return;
        lastState.insert(id, state);
        say(QStringLiteral("[%1] %2 \"%3\": %4").arg(id, state, job.name, job.message));
    });

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    QTimer stopPoll;
    QObject::connect(&stopPoll, &QTimer::timeout, &app, [] {
        if (g_stopRequested.load())
            QCoreApplication::quit();
    });
    stopPoll.start(200);

    const int rc = app.exec();
    say(QStringLiteral("Shutting down."));
    return rc;
}
