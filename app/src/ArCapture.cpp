#include "ArCapture.h"

#include <QCoreApplication>
#include <QDir>
#include <QPermissions>
#include <QQuickImageProvider>
#include <QSettings>
#include <QTimer>

#ifdef U3D_HAS_ARCORE
#include "ArCoreSession.h"
#endif

namespace {

class PreviewProvider : public QQuickImageProvider
{
public:
    PreviewProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage requestImage(const QString &, QSize *size, const QSize &requested) override
    {
#ifdef U3D_HAS_ARCORE
        QImage img = ArCoreSession::latestPreview();
#else
        QImage img;
#endif
        if (img.isNull()) {
            img = QImage(4, 3, QImage::Format_RGB888);
            img.fill(Qt::black);
        }
        if (requested.isValid())
            img = img.scaled(requested, Qt::KeepAspectRatio, Qt::FastTransformation);
        if (size)
            *size = img.size();
        return img;
    }
};

// Set while an ARCore call that could crash the process is in progress. If it is still set at
// start-up, the previous attempt killed the app, so ARCore stays off until the user retries.
const char *kGuardKey = "arcore/inProgress";

// ARCore must deliver camera frames within this time, otherwise the plain camera is used.
constexpr int kFirstFrameTimeoutMs = 8000;

} // namespace

ArCapture::ArCapture(QObject *parent)
    : QObject(parent)
{
    QSettings settings;
    if (settings.value(QLatin1String(kGuardKey), false).toBool()) {
        m_crashedLastTime = true;
        m_ready = true;
        settings.setValue(QLatin1String(kGuardKey), false);
        m_status = tr("ARCore stopped unexpectedly last time, so the normal camera is used.");
        return;
    }
    checkAvailability();
}

ArCapture::~ArCapture()
{
    stop();
    setGuard(false); // closed normally (a crash never gets here, so the guard survives one)
}

QQuickImageProvider *ArCapture::createImageProvider()
{
    return new PreviewProvider();
}

void ArCapture::setGuard(bool risky)
{
    QSettings settings;
    settings.setValue(QLatin1String(kGuardKey), risky);
    settings.sync(); // must reach disk before a possible crash
}

void ArCapture::setStatus(const QString &status)
{
    if (status == m_status)
        return;
    m_status = status;
    emit statusChanged();
}

void ArCapture::setAutoCapture(bool on)
{
    if (on == m_autoCapture)
        return;
    m_autoCapture = on;
#ifdef U3D_HAS_ARCORE
    if (m_session)
        m_session->setAutoCapture(on);
#endif
    emit autoCaptureChanged();
}

void ArCapture::retryArCore()
{
    m_checkAttempts = 0;
    m_crashedLastTime = false;
    m_ready = false;
    setStatus(QString());
    emit availableChanged();
    checkAvailability();
}

void ArCapture::checkAvailability()
{
    if (m_crashedLastTime)
        return;
#ifdef U3D_HAS_ARCORE
    setGuard(true);
    ArCoreSession::checkAvailabilityAsync(this, [this](ArCoreSession::Availability availability) {
        setGuard(false);
        if (availability == ArCoreSession::Availability::Checking && ++m_checkAttempts < 10) {
            // The first check runs asynchronously inside ARCore; ask again shortly. Without
            // Google Play Services (e.g. many phones sold in China) it never finishes, so give
            // up after ~5 s - the normal camera does not depend on this check.
            QTimer::singleShot(500, this, &ArCapture::checkAvailability);
            return;
        }
        m_available = availability == ArCoreSession::Availability::Supported
                      || availability == ArCoreSession::Availability::NeedsInstall;
        m_ready = true;
        emit availableChanged();
    });
#else
    m_ready = true;
    emit availableChanged();
#endif
}

void ArCapture::start(const QString &projectPath)
{
    if (m_session || m_starting)
        return;
#if QT_CONFIG(permissions)
    // The capture page requests the permission; ARCore only starts once it is granted.
    if (qApp->checkPermission(QCameraPermission()) != Qt::PermissionStatus::Granted) {
        emit failed(tr("Camera permission is required to capture."));
        return;
    }
#endif
#ifdef U3D_HAS_ARCORE
    m_starting = true;
    setStatus(tr("Starting ARCore..."));
    setGuard(true);
    ArCoreSession::requestInstallAsync(this, [this, projectPath](bool installed, const QString &message) {
        setGuard(false);
        m_starting = false;
        if (!installed) {
            setStatus(message);
            emit failed(message);
            return;
        }
        startSession(projectPath);
    });
#else
    Q_UNUSED(projectPath)
    emit failed(tr("ARCore is not available in this build."));
#endif
}

void ArCapture::startSession(const QString &projectPath)
{
#ifdef U3D_HAS_ARCORE
    m_frames = int(QDir(QDir(projectPath).filePath(QStringLiteral("images")))
                       .entryList({QStringLiteral("*.jpg")}, QDir::Files).size());
    m_points = 0;
    emit framesChanged();
    m_session = new ArCoreSession(projectPath);
    m_session->setAutoCapture(m_autoCapture);
    // The thread cleans itself up when it ends, even if this object is gone by then.
    connect(m_session, &QThread::finished, m_session, &QObject::deleteLater);
    connect(m_session, &ArCoreSession::trackingChanged, this, [this](bool tracking, const QString &msg) {
        if (tracking && !m_tracking)
            setGuard(false); // ARCore is up and running
        m_tracking = tracking;
        m_status = msg;
        emit statusChanged();
    });
    connect(m_session, &ArCoreSession::frameSaved, this, [this](int frames, int points) {
        m_frames = frames;
        m_points = points;
        emit framesChanged();
    });
    connect(m_session, &ArCoreSession::previewUpdated, this, [this] {
        ++m_previewRevision;
        emit previewChanged();
    });
    connect(m_session, &ArCoreSession::failed, this, [this](const QString &msg) {
        setStatus(msg);
        stop();
        emit failed(msg);
    });
    setGuard(true); // cleared once tracking starts or the session stops normally
    m_session->start();
    emit runningChanged();

    // Watchdog: no camera frames -> give up on ARCore for this capture.
    const int revision = m_previewRevision;
    QTimer::singleShot(kFirstFrameTimeoutMs, this, [this, revision] {
        if (m_session && m_previewRevision == revision) {
            const QString msg = tr("ARCore did not deliver camera images; switched to the normal camera.");
            setStatus(msg);
            stop();
            emit failed(msg);
        }
    });
#else
    Q_UNUSED(projectPath)
#endif
}

void ArCapture::stop()
{
#ifdef U3D_HAS_ARCORE
    if (!m_session)
        return;
    // Never wait here: the capture thread may itself be waiting for the Android UI thread.
    // It finishes on its own (writing arcore/points.ply) and deletes itself.
    ArCoreSession *session = m_session;
    m_session = nullptr;
    disconnect(session, nullptr, this, nullptr);
    session->requestStop();
    setGuard(false);
    m_tracking = false;
    emit statusChanged();
    emit runningChanged();
#endif
}

void ArCapture::captureNow()
{
#ifdef U3D_HAS_ARCORE
    if (m_session)
        m_session->requestCapture();
#endif
}
