#include "ArCapture.h"

#include <QCoreApplication>
#include <QDir>
#include <QPermissions>
#include <QQuickImageProvider>
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

} // namespace

ArCapture::ArCapture(QObject *parent)
    : QObject(parent)
{
    checkAvailability();
}

ArCapture::~ArCapture()
{
    stop();
}

QQuickImageProvider *ArCapture::createImageProvider()
{
    return new PreviewProvider();
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

void ArCapture::checkAvailability()
{
#ifdef U3D_HAS_ARCORE
    const auto availability = ArCoreSession::checkAvailability();
    if (availability == ArCoreSession::Availability::Checking) {
        // The check runs asynchronously the first time; ask again shortly.
        QTimer::singleShot(300, this, &ArCapture::checkAvailability);
        return;
    }
    const bool available = availability == ArCoreSession::Availability::Supported
                           || availability == ArCoreSession::Availability::NeedsInstall;
    if (available != m_available) {
        m_available = available;
        emit availableChanged();
    }
#endif
}

void ArCapture::start(const QString &projectPath)
{
    if (m_session)
        return;
#if QT_CONFIG(permissions)
    QCameraPermission permission;
    switch (qApp->checkPermission(permission)) {
    case Qt::PermissionStatus::Undetermined:
        qApp->requestPermission(permission, this, [this, projectPath](const QPermission &p) {
            if (p.status() == Qt::PermissionStatus::Granted)
                startSession(projectPath);
            else
                emit failed(tr("Camera permission is required to capture."));
        });
        return;
    case Qt::PermissionStatus::Denied:
        emit failed(tr("Camera permission was denied. Enable it in the system settings."));
        return;
    case Qt::PermissionStatus::Granted:
        break;
    }
#endif
    startSession(projectPath);
}

void ArCapture::startSession(const QString &projectPath)
{
#ifdef U3D_HAS_ARCORE
    QString message;
    if (!ArCoreSession::requestInstall(&message)) {
        setStatus(message);
        emit failed(message);
        return;
    }
    m_frames = int(QDir(QDir(projectPath).filePath(QStringLiteral("images"))).entryList({QStringLiteral("*.jpg")}, QDir::Files).size());
    m_points = 0;
    emit framesChanged();
    m_session = new ArCoreSession(projectPath, this);
    m_session->setAutoCapture(m_autoCapture);
    connect(m_session, &ArCoreSession::trackingChanged, this, [this](bool tracking, const QString &msg) {
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
        emit failed(msg);
        stop();
    });
    setStatus(tr("Starting ARCore..."));
    m_session->start();
    emit runningChanged();
#else
    Q_UNUSED(projectPath)
    emit failed(tr("ARCore is not available in this build."));
#endif
}

void ArCapture::stop()
{
#ifdef U3D_HAS_ARCORE
    if (!m_session)
        return;
    ArCoreSession *session = m_session;
    m_session = nullptr;
    session->requestStop();
    session->wait(); // writes arcore/points.ply before returning
    session->deleteLater();
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
