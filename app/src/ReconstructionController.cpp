#include "ReconstructionController.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QtConcurrent>

#ifdef U3D_HAS_RECONSTRUCTION
#include "u3d/Reconstructor.h"
#endif

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniObject>
#endif

ReconstructionController::ReconstructionController(QObject *parent)
    : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<QString>::finished, this, [this] {
        m_error = m_watcher.result();
        const bool ok = m_error.isEmpty();
        const QString ply = QDir(m_projectPath).filePath(QStringLiteral("output/splat.ply"));
        m_resultUrl = ok && QFileInfo::exists(ply) ? QUrl::fromLocalFile(ply) : QUrl();
        appendLog(ok ? QStringLiteral("Finished.") : QStringLiteral("Failed: ") + m_error);
        m_running = false;
        setKeepScreenOn(false);
        emit runningChanged();
        emit finished(ok);
    });
}

ReconstructionController::~ReconstructionController()
{
    if (m_cancel)
        m_cancel->store(true);
    m_watcher.waitForFinished();
}

void ReconstructionController::appendLog(const QString &line)
{
    m_log += line + QLatin1Char('\n');
    if (m_log.size() > 40000)
        m_log = m_log.right(30000);
    QFile file(QDir(m_projectPath).filePath(QStringLiteral("log.txt")));
    if (file.open(QIODevice::Append))
        file.write((line + QLatin1Char('\n')).toUtf8());
    emit logChanged();
}

bool ReconstructionController::start(const QString &projectPath, int quality, bool forceSfm)
{
    if (m_running)
        return false;
#ifndef U3D_HAS_RECONSTRUCTION
    Q_UNUSED(quality)
    Q_UNUSED(forceSfm)
    m_projectPath = projectPath;
    m_error = tr("This build cannot reconstruct (viewer only).");
    emit finished(false);
    return false;
#else
    m_projectPath = projectPath;
    m_running = true;
    m_progress = 0;
    m_stage.clear();
    m_message = tr("Starting...");
    m_log.clear();
    m_error.clear();
    m_resultUrl.clear();
    m_cancel = std::make_shared<std::atomic_bool>(false);
    emit runningChanged();
    emit progressChanged();
    appendLog(QStringLiteral("=== %1 ===").arg(QDateTime::currentDateTime().toString(Qt::ISODate)));
    setKeepScreenOn(true);

    u3d::ReconstructOptions options;
    options.quality = quality == Fast ? u3d::Quality::Fast : quality == High ? u3d::Quality::High : u3d::Quality::Balanced;
    options.forceSfm = forceSfm;

    QPointer<ReconstructionController> self(this);
    auto cancel = m_cancel;
    const std::string dir = QDir::toNativeSeparators(projectPath).toStdString();
    m_watcher.setFuture(QtConcurrent::run([self, cancel, dir, options]() -> QString {
        u3d::ReconstructResult result;
        std::string error;
        const bool ok = u3d::reconstructProject(
            dir, options, result,
            [self](const u3d::Progress &p) {
                const QString stage = QString::fromStdString(p.stage), message = QString::fromStdString(p.message);
                const double fraction = p.fraction;
                QMetaObject::invokeMethod(self, [self, stage, message, fraction] {
                    if (!self)
                        return;
                    self->m_progress = fraction;
                    self->m_stage = stage;
                    self->m_message = message;
                    emit self->progressChanged();
                }, Qt::QueuedConnection);
            },
            [self](const std::string &line) {
                const QString text = QString::fromStdString(line);
                QMetaObject::invokeMethod(self, [self, text] {
                    if (self)
                        self->appendLog(text);
                }, Qt::QueuedConnection);
            },
            cancel.get(), &error);
        return ok ? QString() : QString::fromStdString(error.empty() ? "Unknown error" : error);
    }));
    return true;
#endif
}

void ReconstructionController::cancel()
{
    if (m_cancel)
        m_cancel->store(true);
    m_message = tr("Cancelling...");
    emit progressChanged();
}

void ReconstructionController::setKeepScreenOn(bool on)
{
#ifdef Q_OS_ANDROID
    // Training takes minutes; keep the screen (and the CPU at full speed) awake meanwhile.
    QNativeInterface::QAndroidApplication::runOnAndroidMainThread([on] {
        QJniObject activity = QNativeInterface::QAndroidApplication::context();
        if (!activity.isValid())
            return;
        QJniObject window = activity.callObjectMethod("getWindow", "()Landroid/view/Window;");
        if (!window.isValid())
            return;
        const jint FLAG_KEEP_SCREEN_ON = 0x00000080;
        if (on)
            window.callMethod<void>("addFlags", "(I)V", FLAG_KEEP_SCREEN_ON);
        else
            window.callMethod<void>("clearFlags", "(I)V", FLAG_KEEP_SCREEN_ON);
    });
#else
    Q_UNUSED(on)
#endif
}
