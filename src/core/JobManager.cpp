#include "JobManager.h"

#include "ImageOrientation.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>

#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif

namespace u3d {

using pipeline::Stage;

namespace {

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

QString defaultJobName()
{
    return QStringLiteral("Scan ") + QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm"));
}

} // namespace

JobManager::JobManager(const Settings &settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
    , m_jobsDir(QDir(settings.dataDir).filePath(QStringLiteral("jobs")))
{
    QDir().mkpath(m_jobsDir);
    loadExisting();
    QTimer::singleShot(0, this, &JobManager::scheduleNext);
}

JobManager::~JobManager()
{
    if (!m_run)
        return;
    m_run->cancel->store(true);
    if (m_run->process) {
        disconnect(m_run->process, nullptr, this, nullptr);
        killProcessTree(m_run->process);
        if (!m_run->process->waitForFinished(3000))
            m_run->process->kill();
        m_run->process->waitForFinished(1000);
    }
    if (m_run->prepareThread) {
        disconnect(m_run->prepareThread, nullptr, this, nullptr);
        m_run->prepareThread->wait();
        delete m_run->prepareThread;
    }
    auto it = m_jobs.find(m_run->jobId);
    if (it != m_jobs.end()) {
        it->status = JobStatus::Failed;
        it->message = QStringLiteral("Interrupted: the application was closed while running.");
        saveJob(*it, true);
    }
}

void JobManager::setSettings(const Settings &settings)
{
    const QString dataDir = m_settings.dataDir;
    m_settings = settings;
    m_settings.dataDir = dataDir;
}

// ----------------------------------------------------------------------------- persistence

pipeline::JobPaths JobManager::paths(const QString &id) const
{
    return {QDir(m_jobsDir).filePath(id)};
}

QString JobManager::jobDir(const QString &id) const { return paths(id).root; }

void JobManager::loadExisting()
{
    const QFileInfoList dirs = QDir(m_jobsDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    QList<Job> queued;
    for (const QFileInfo &dir : dirs) {
        QFile meta(QDir(dir.absoluteFilePath()).filePath(QStringLiteral("job.json")));
        if (!meta.open(QIODevice::ReadOnly))
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(meta.readAll());
        if (!doc.isObject())
            continue;
        Job job = Job::fromJson(doc.object());
        if (job.id != dir.fileName())
            continue;
        if (job.status == JobStatus::Running) {
            job.status = JobStatus::Failed;
            job.message = QStringLiteral("Interrupted: the application stopped while this job was running.");
            saveJob(job, true);
        } else if (job.status == JobStatus::Uploading) {
            job.status = JobStatus::Failed;
            job.message = QStringLiteral("The upload was not completed.");
            saveJob(job, true);
        } else if (job.status == JobStatus::Queued) {
            queued << job;
        }
        m_jobs.insert(job.id, job);
    }
    std::sort(queued.begin(), queued.end(),
              [](const Job &a, const Job &b) { return a.createdAt < b.createdAt; });
    for (const Job &job : queued)
        m_queue.enqueue(job.id);
}

void JobManager::saveJob(Job &job, bool force)
{
    const qint64 now = nowMs();
    job.updatedAt = now;
    if (!force && now - m_lastSaved.value(job.id, 0) < 2000)
        return;
    m_lastSaved.insert(job.id, now);
    const pipeline::JobPaths p = paths(job.id);
    if (!QFileInfo::exists(p.root))
        return;
    QSaveFile file(p.meta());
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(job.toJson()).toJson(QJsonDocument::Indented));
        file.commit();
    }
}

void JobManager::updateJob(const QString &id, bool forceSave)
{
    auto it = m_jobs.find(id);
    if (it == m_jobs.end())
        return;
    saveJob(*it, forceSave);
    emit jobChanged(id);
}

void JobManager::appendLog(const QString &id, const QString &text)
{
    if (m_run && m_run->jobId == id && m_run->log.isOpen()) {
        m_run->log.write(text.toUtf8());
        m_run->log.flush();
    } else {
        QFile log(paths(id).log());
        if (log.open(QIODevice::Append))
            log.write(text.toUtf8());
    }
    emit logAppended(id, text);
}

// ----------------------------------------------------------------------------- creating jobs

QString JobManager::createJob(const QString &name, const QString &method, int iterations,
                              QString *error)
{
    const QString m = method.isEmpty() ? m_settings.defaultMethod : method;
    if (!pipeline::isValidMethod(m)) {
        if (error)
            *error = QStringLiteral("Unknown method '%1'. Choose one of: %2")
                         .arg(m, pipeline::methods().join(QStringLiteral(", ")));
        return {};
    }
    const int iters = iterations > 0 ? iterations : m_settings.defaultIterations;
    if (iters < 100 || iters > 1000000) {
        if (error)
            *error = QStringLiteral("Iterations must be between 100 and 1000000.");
        return {};
    }

    Job job;
    job.id = QUuid::createUuid().toString(QUuid::Id128).left(12);
    job.name = name.trimmed().left(120);
    if (job.name.isEmpty())
        job.name = defaultJobName();
    job.method = m;
    job.iterations = iters;
    job.status = JobStatus::Uploading;
    job.stage = QStringLiteral("uploading");
    job.message = QStringLiteral("Waiting for input files");
    job.createdAt = nowMs();

    if (!QDir().mkpath(paths(job.id).input())) {
        if (error)
            *error = QStringLiteral("Cannot create job folder in %1").arg(m_jobsDir);
        return {};
    }
    m_jobs.insert(job.id, job);
    saveJob(m_jobs[job.id], true);
    emit jobAdded(job.id);
    return job.id;
}

QString JobManager::addInput(const QString &id, const QString &fileName, const QByteArray &data,
                             QString *error)
{
    auto fail = [error](const QString &msg) {
        if (error)
            *error = msg;
        return QString();
    };
    auto it = m_jobs.find(id);
    if (it == m_jobs.end())
        return fail(QStringLiteral("Job not found."));
    if (it->status != JobStatus::Uploading)
        return fail(QStringLiteral("Inputs can only be added before the job is started."));

    const QString safe = pipeline::sanitizeFileName(
        fileName, QStringLiteral("input_%1").arg(it->numInputs, 4, 10, QLatin1Char('0')));
    if (!pipeline::isImageFile(safe) && !pipeline::isVideoFile(safe))
        return fail(QStringLiteral("Unsupported file type: %1").arg(safe));

    const QDir inputDir(paths(id).input());
    const QFileInfo info(safe);
    QString target = info.completeBaseName() + QLatin1Char('.') + info.suffix().toLower();
    for (int n = 1; inputDir.exists(target); ++n)
        target = QStringLiteral("%1_%2.%3").arg(info.completeBaseName()).arg(n).arg(info.suffix().toLower());

    QSaveFile file(inputDir.filePath(target));
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        return fail(QStringLiteral("Cannot write %1").arg(target));

    it->numInputs += 1;
    it->message = QStringLiteral("Received %1 file(s)").arg(it->numInputs);
    updateJob(id, false);
    return target;
}

QString JobManager::addInputFile(const QString &id, const QString &sourcePath, QString *error)
{
    QFile src(sourcePath);
    if (!src.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Cannot read %1").arg(sourcePath);
        return {};
    }
    // Images are a few MB; videos are read in one go too, which keeps this simple.
    return addInput(id, QFileInfo(sourcePath).fileName(), src.readAll(), error);
}

bool JobManager::startJob(const QString &id, QString *error)
{
    auto it = m_jobs.find(id);
    if (it == m_jobs.end()) {
        if (error)
            *error = QStringLiteral("Job not found.");
        return false;
    }
    if (it->status != JobStatus::Uploading) {
        if (error)
            *error = QStringLiteral("Job was already started.");
        return false;
    }
    const QStringList files = QDir(paths(id).input()).entryList(QDir::Files, QDir::Name);
    QString classifyError;
    const QString inputType = pipeline::classifyInputs(files, &classifyError);
    if (inputType.isEmpty()) {
        if (error)
            *error = classifyError;
        return false;
    }
    it->inputType = inputType;
    it->numInputs = files.size();
    it->status = JobStatus::Queued;
    it->stage = QStringLiteral("queued");
    it->message = QStringLiteral("Waiting in queue");
    updateJob(id, true);
    m_queue.enqueue(id);
    QTimer::singleShot(0, this, &JobManager::scheduleNext);
    return true;
}

QString JobManager::submit(const QString &name, const QString &method, int iterations,
                           const QStringList &files, QString *error)
{
    const QString id = createJob(name, method, iterations, error);
    if (id.isEmpty())
        return {};
    for (const QString &file : files) {
        if (addInputFile(id, file, error).isEmpty()) {
            removeJob(id);
            return {};
        }
    }
    if (!startJob(id, error)) {
        removeJob(id);
        return {};
    }
    return id;
}

// ----------------------------------------------------------------------------- queries

QList<Job> JobManager::jobs() const
{
    QList<Job> list = m_jobs.values();
    std::sort(list.begin(), list.end(),
              [](const Job &a, const Job &b) { return a.createdAt > b.createdAt; });
    return list;
}

QString JobManager::logTail(const QString &id, qint64 maxBytes) const
{
    QFile log(paths(id).log());
    if (!log.open(QIODevice::ReadOnly))
        return {};
    const qint64 size = log.size();
    if (size > maxBytes)
        log.seek(size - maxBytes);
    return QString::fromUtf8(log.readAll());
}

QString JobManager::resultPath(const QString &id) const
{
    const Job j = m_jobs.value(id);
    if (!j.hasResult())
        return {};
    const QString path = QDir(paths(id).root).filePath(j.resultFile);
    return QFileInfo::exists(path) ? path : QString();
}

// ----------------------------------------------------------------------------- cancel / remove

bool JobManager::cancelJob(const QString &id)
{
    auto it = m_jobs.find(id);
    if (it == m_jobs.end())
        return false;
    if (it->isFinished())
        return true;
    if (it->status == JobStatus::Uploading || it->status == JobStatus::Queued) {
        m_queue.removeAll(id);
        it->status = JobStatus::Cancelled;
        it->message = QStringLiteral("Cancelled before start");
        updateJob(id, true);
        return true;
    }
    if (m_run && m_run->jobId == id) {
        m_run->cancel->store(true);
        it->message = QStringLiteral("Cancelling...");
        updateJob(id, true);
        if (m_run->process)
            killProcessTree(m_run->process);
    }
    return true;
}

bool JobManager::removeJob(const QString &id)
{
    if (!m_jobs.contains(id))
        return false;
    if (m_run && m_run->jobId == id) {
        m_removeWhenStopped.insert(id);
        cancelJob(id);
        return true;
    }
    deleteJobFiles(id);
    return true;
}

void JobManager::deleteJobFiles(const QString &id)
{
    m_queue.removeAll(id);
    m_jobs.remove(id);
    m_lastSaved.remove(id);
    m_removeWhenStopped.remove(id);
    QDir(paths(id).root).removeRecursively();
    emit jobRemoved(id);
}

void JobManager::killProcessTree(QProcess *process)
{
    if (!process || process->state() == QProcess::NotRunning)
        return;
    const qint64 pid = process->processId();
#if defined(Q_OS_WIN)
    // Kill the whole tree (ns-train spawns python workers; ns-process-data spawns COLMAP).
    QProcess::startDetached(QStringLiteral("taskkill"),
                            {QStringLiteral("/T"), QStringLiteral("/F"), QStringLiteral("/PID"),
                             QString::number(pid)});
#elif QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Each tool runs in its own session (see startStage), so its process group can be killed.
    ::kill(-pid_t(pid), SIGTERM);
    QPointer<QProcess> guard(process);
    QTimer::singleShot(10000, this, [guard, pid] {
        if (guard && guard->state() != QProcess::NotRunning)
            ::kill(-pid_t(pid), SIGKILL);
    });
#else
    Q_UNUSED(pid)
    process->terminate();
    QPointer<QProcess> guard(process);
    QTimer::singleShot(10000, this, [guard] {
        if (guard && guard->state() != QProcess::NotRunning)
            guard->kill();
    });
#endif
}

// ----------------------------------------------------------------------------- running

void JobManager::scheduleNext()
{
    if (m_run)
        return;
    while (!m_queue.isEmpty()) {
        const QString id = m_queue.dequeue();
        auto it = m_jobs.find(id);
        if (it != m_jobs.end() && it->status == JobStatus::Queued) {
            beginRun(id);
            return;
        }
    }
}

void JobManager::beginRun(const QString &id)
{
    Job &job = m_jobs[id];
    job.status = JobStatus::Running;
    job.progress = 0.0;
    m_run = std::make_unique<Run>();
    m_run->jobId = id;
    m_run->log.setFileName(paths(id).log());
    m_run->log.open(QIODevice::Append);
    appendLog(id, QStringLiteral("[u3d] Job %1 (%2, %3 %4 input(s), %5 iterations)%6\n")
                      .arg(job.name, job.method)
                      .arg(job.numInputs)
                      .arg(job.inputType)
                      .arg(job.iterations)
                      .arg(m_settings.dryRun ? QStringLiteral(" [dry run]") : QString()));

    if (job.inputType == QLatin1String("images"))
        startPrepare();
    else
        startStage(Stage::ProcessData);
}

void JobManager::setStage(Stage stage)
{
    Job &job = m_jobs[m_run->jobId];
    job.stage = pipeline::stageName(stage);
    job.progress = pipeline::stageRange(stage).first;
    job.message = pipeline::stageDescription(stage, job.method);
    m_run->stage = stage;
    updateJob(job.id, true);
}

void JobManager::startPrepare()
{
    setStage(Stage::Prepare);
    const QString inputDir = paths(m_run->jobId).input();
    auto cancel = m_run->cancel;
    auto rewritten = std::make_shared<int>(0);
    QThread *thread = QThread::create([inputDir, cancel, rewritten] {
        *rewritten = normalizeImageOrientation(inputDir, cancel.get());
    });
    m_run->prepareThread = thread;
    connect(thread, &QThread::finished, this, [this, thread, rewritten] {
        thread->deleteLater();
        if (!m_run || m_run->prepareThread != thread)
            return;
        m_run->prepareThread = nullptr;
        if (m_run->cancel->load()) {
            finishRun(JobStatus::Cancelled, QStringLiteral("Cancelled"));
            return;
        }
        if (*rewritten > 0)
            appendLog(m_run->jobId,
                      QStringLiteral("[u3d] Applied EXIF orientation to %1 image(s)\n").arg(*rewritten));
        startStage(Stage::ProcessData);
    });
    thread->start();
}

void JobManager::startStage(Stage stage)
{
    setStage(stage);
    const Job &job = m_jobs[m_run->jobId];
    const pipeline::JobPaths p = paths(job.id);

    pipeline::Command cmd;
    switch (stage) {
    case Stage::ProcessData:
        cmd = pipeline::processDataCommand(m_settings, p, job.inputType);
        break;
    case Stage::Train:
        cmd = pipeline::trainCommand(m_settings, p, job.id, job.method, job.iterations);
        break;
    case Stage::Export: {
        const QString config = pipeline::findTrainConfig(p.outputs());
        if (config.isEmpty()) {
            finishRun(JobStatus::Failed, QStringLiteral("ns-train did not produce a config.yml."));
            return;
        }
        cmd = pipeline::exportCommand(m_settings, p, job.method, config);
        break;
    }
    case Stage::Prepare:
        return;
    }

    appendLog(job.id, QStringLiteral("\n[u3d] $ %1\n").arg(cmd.toString()));

    auto *process = new QProcess(this);
    process->setWorkingDirectory(p.root);
    process->setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    env.insert(QStringLiteral("COLUMNS"), QStringLiteral("200"));
    env.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));
    env.insert(QStringLiteral("NO_COLOR"), QStringLiteral("1"));
    if (!m_settings.nsBinDir.isEmpty() && !m_settings.dryRun) {
        // ns-process-data looks up colmap/ffmpeg on PATH; they usually live in the same env.
        env.insert(QStringLiteral("PATH"), QDir::toNativeSeparators(m_settings.nsBinDir)
                                               + QDir::listSeparator()
                                               + env.value(QStringLiteral("PATH")));
    }
    process->setProcessEnvironment(env);
#if defined(Q_OS_UNIX) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    process->setChildProcessModifier([] { ::setsid(); });
#endif
    connect(process, &QProcess::readyRead, this, &JobManager::onProcessOutput);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            &JobManager::onProcessFinished);
    connect(process, &QProcess::errorOccurred, this, &JobManager::onProcessError);
    m_run->process = process;
    m_run->pendingOutput.clear();
    process->start(cmd.program, cmd.arguments);
}

void JobManager::onProcessOutput()
{
    if (!m_run || m_run->process != sender())
        return;
    m_run->pendingOutput += m_run->process->readAll();
    QByteArray &buf = m_run->pendingOutput;
    int start = 0;
    for (int i = 0; i < buf.size(); ++i) {
        const char c = buf.at(i);
        if (c == '\n' || c == '\r') {
            if (i > start)
                handleOutputLine(QString::fromUtf8(buf.constData() + start, i - start));
            start = i + 1;
        }
    }
    buf.remove(0, start);
}

void JobManager::handleOutputLine(const QString &rawLine)
{
    const QString line = pipeline::stripAnsi(rawLine);
    appendLog(m_run->jobId, line + QLatin1Char('\n'));
    if (m_run->stage != Stage::Train)
        return;
    const double fraction = pipeline::parseTrainProgress(line);
    if (fraction < 0)
        return;
    const auto range = pipeline::stageRange(Stage::Train);
    Job &job = m_jobs[m_run->jobId];
    job.progress = range.first + (range.second - range.first) * fraction;
    job.message = QStringLiteral("Training %1: %2%").arg(job.method).arg(fraction * 100.0, 0, 'f', 1);
    updateJob(job.id, false);
}

void JobManager::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    if (!m_run || m_run->process != sender())
        return;
    if (!m_run->pendingOutput.isEmpty()) {
        handleOutputLine(QString::fromUtf8(m_run->pendingOutput));
        m_run->pendingOutput.clear();
    }
    m_run->process->deleteLater();
    m_run->process = nullptr;

    if (m_run->cancel->load()) {
        finishRun(JobStatus::Cancelled, QStringLiteral("Cancelled"));
        return;
    }
    if (status != QProcess::NormalExit || exitCode != 0) {
        finishRun(JobStatus::Failed,
                  QStringLiteral("Stage '%1' failed (exit code %2). See the log for details.")
                      .arg(pipeline::stageName(m_run->stage))
                      .arg(exitCode));
        return;
    }

    switch (m_run->stage) {
    case Stage::ProcessData:
        startStage(Stage::Train);
        return;
    case Stage::Train:
        startStage(Stage::Export);
        return;
    case Stage::Export: {
        const pipeline::JobPaths p = paths(m_run->jobId);
        const QString result = pipeline::findExport(p.exports());
        if (result.isEmpty()) {
            finishRun(JobStatus::Failed, QStringLiteral("ns-export did not produce a .ply file."));
            return;
        }
        m_jobs[m_run->jobId].resultFile = QDir(p.root).relativeFilePath(result);
        appendLog(m_run->jobId, QStringLiteral("\n[u3d] Result: %1\n").arg(result));
        finishRun(JobStatus::Succeeded, QStringLiteral("Reconstruction complete"));
        return;
    }
    case Stage::Prepare:
        return;
    }
}

void JobManager::onProcessError(QProcess::ProcessError error)
{
    if (!m_run || m_run->process != sender() || error != QProcess::FailedToStart)
        return; // other errors are followed by finished()
    const QString program = m_run->process->program();
    m_run->process->deleteLater();
    m_run->process = nullptr;
    finishRun(JobStatus::Failed,
              QStringLiteral("Could not start '%1'. Is Nerfstudio installed? Set the Nerfstudio "
                             "bin folder in Preferences (or U3D_NS_BIN_DIR), or enable dry-run mode.")
                  .arg(program));
}

void JobManager::finishRun(JobStatus status, const QString &message)
{
    const QString id = m_run->jobId;
    auto it = m_jobs.find(id);
    if (it != m_jobs.end()) {
        it->status = status;
        it->message = message;
        if (status == JobStatus::Succeeded) {
            it->stage = QStringLiteral("done");
            it->progress = 1.0;
        }
        appendLog(id, QStringLiteral("\n[u3d] %1: %2\n").arg(jobStatusToString(status), message));
        saveJob(*it, true);
    }
    m_run.reset();

    if (m_removeWhenStopped.contains(id))
        deleteJobFiles(id);
    else if (it != m_jobs.end())
        emit jobChanged(id);

    QTimer::singleShot(0, this, &JobManager::scheduleNext);
}

} // namespace u3d
