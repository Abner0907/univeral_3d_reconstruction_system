#pragma once

#include <QImage>
#include <QString>
#include <QThread>

#include <atomic>

class QOffscreenSurface;
class QOpenGLContext;

// ARCore capture session (Android only). ARCore is loaded at runtime with dlopen from the
// com.google.ar:core AAR, so phones without ARCore simply report it as unavailable.
//
// While running, every keyframe saves into the project:
//   images/IMG_####.jpg      full-resolution camera image (sensor orientation)
//   arcore/cameras.txt       intrinsics + tracked pose (OpenCV convention, see u3d/Project.h)
//   arcore/points.ply        accumulated ARCore feature points (written on stop)
class ArCoreSession : public QThread
{
    Q_OBJECT

public:
    enum class Availability { Unsupported, Supported, NeedsInstall, Checking };

    static Availability checkAvailability();
    // Starts Google Play Services for AR installation if needed; returns true once installed.
    static bool requestInstall(QString *message);

    explicit ArCoreSession(const QString &projectPath, QObject *parent = nullptr);
    ~ArCoreSession() override;

    void requestCapture() { m_captureRequested.store(true); }
    void setAutoCapture(bool on) { m_autoCapture.store(on); }
    void requestStop() { m_stop.store(true); }

    static QImage latestPreview();

signals:
    void trackingChanged(bool tracking, const QString &message);
    void frameSaved(int frames, int points);
    void previewUpdated();
    void failed(const QString &message);

protected:
    void run() override;

private:
    QString m_projectPath;
    QOffscreenSurface *m_surface = nullptr;
    QOpenGLContext *m_context = nullptr;
    std::atomic_bool m_captureRequested{false};
    std::atomic_bool m_autoCapture{false};
    std::atomic_bool m_stop{false};
};
