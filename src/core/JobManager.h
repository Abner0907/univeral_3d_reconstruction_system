#pragma once

#include "Job.h"
#include "Pipeline.h"
#include "Settings.h"

#include <QFile>
#include <QHash>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QQueue>
#include <QSet>

#include <atomic>
#include <memory>

class QThread;

namespace u3d {

// Owns all reconstruction jobs and runs them one at a time (one GPU) by driving the
// Nerfstudio command-line tools with QProcess. Everything happens on the thread that owns the
// manager (the GUI/main thread); only the EXIF-normalisation step uses a short-lived worker.
//
// On disk, every job lives in <dataDir>/jobs/<id>/ (job.json, input/, processed/, outputs/,
// exports/, log.txt) so jobs survive restarts.
class JobManager : public QObject
{
    Q_OBJECT

public:
    explicit JobManager(const Settings &settings, QObject *parent = nullptr);
    ~JobManager() override;

    Settings settings() const { return m_settings; }
    // Applies to jobs started afterwards. The data directory cannot change at runtime.
    void setSettings(const Settings &settings);

    // --- creating jobs: createJob -> addInput... -> startJob
    QString createJob(const QString &name, const QString &method, int iterations,
                      QString *error = nullptr);
    // Stores one input file; returns the stored file name (or empty on error).
    QString addInput(const QString &id, const QString &fileName, const QByteArray &data,
                     QString *error = nullptr);
    QString addInputFile(const QString &id, const QString &sourcePath, QString *error = nullptr);
    bool startJob(const QString &id, QString *error = nullptr);
    // Convenience for local files: create + copy + start. Returns the job id or empty.
    QString submit(const QString &name, const QString &method, int iterations,
                   const QStringList &files, QString *error = nullptr);

    bool cancelJob(const QString &id);
    // Deletes the job and its files (a running job is cancelled first).
    bool removeJob(const QString &id);

    QList<Job> jobs() const; // newest first
    bool hasJob(const QString &id) const { return m_jobs.contains(id); }
    Job job(const QString &id) const { return m_jobs.value(id); }
    QString jobDir(const QString &id) const;
    QString logTail(const QString &id, qint64 maxBytes = 64 * 1024) const;
    // Absolute path of the result .ply, or empty.
    QString resultPath(const QString &id) const;

signals:
    void jobAdded(const QString &id);
    void jobChanged(const QString &id);
    void jobRemoved(const QString &id);
    void logAppended(const QString &id, const QString &text);

private:
    struct Run {
        QString jobId;
        pipeline::Stage stage = pipeline::Stage::Prepare;
        QProcess *process = nullptr;
        QThread *prepareThread = nullptr;
        std::shared_ptr<std::atomic_bool> cancel = std::make_shared<std::atomic_bool>(false);
        QByteArray pendingOutput;
        QFile log;
    };

    pipeline::JobPaths paths(const QString &id) const;
    void loadExisting();
    void saveJob(Job &job, bool force);
    void updateJob(const QString &id, bool forceSave);
    void appendLog(const QString &id, const QString &text);

    void scheduleNext();
    void beginRun(const QString &id);
    void startPrepare();
    void startStage(pipeline::Stage stage);
    void onProcessOutput();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError error);
    void handleOutputLine(const QString &line);
    void finishRun(JobStatus status, const QString &message);
    void setStage(pipeline::Stage stage);
    void killProcessTree(QProcess *process);
    void deleteJobFiles(const QString &id);

    Settings m_settings;
    QString m_jobsDir;
    QHash<QString, Job> m_jobs;
    QQueue<QString> m_queue;
    QSet<QString> m_removeWhenStopped;
    QHash<QString, qint64> m_lastSaved;
    std::unique_ptr<Run> m_run;
};

} // namespace u3d
