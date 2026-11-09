#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QString>

#include <memory>

class QQuickImageProvider;
class ArCoreSession;

// QML front end of the ARCore capture (Android). On platforms or phones without ARCore,
// `available` is false and the capture page falls back to the Qt Multimedia camera.
// The live preview is served by the image provider "image://arcore/<previewRevision>".
class ArCapture : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(bool tracking READ tracking NOTIFY statusChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(int frameCount READ frameCount NOTIFY framesChanged)
    Q_PROPERTY(int pointCount READ pointCount NOTIFY framesChanged)
    Q_PROPERTY(bool autoCapture READ autoCapture WRITE setAutoCapture NOTIFY autoCaptureChanged)
    Q_PROPERTY(int previewRevision READ previewRevision NOTIFY previewChanged)

public:
    explicit ArCapture(QObject *parent = nullptr);
    ~ArCapture() override;

    bool available() const { return m_available; }
    bool running() const { return m_session != nullptr; }
    bool tracking() const { return m_tracking; }
    QString status() const { return m_status; }
    int frameCount() const { return m_frames; }
    int pointCount() const { return m_points; }
    bool autoCapture() const { return m_autoCapture; }
    void setAutoCapture(bool on);
    int previewRevision() const { return m_previewRevision; }

    Q_INVOKABLE void checkAvailability();
    Q_INVOKABLE void start(const QString &projectPath);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void captureNow();

    static QQuickImageProvider *createImageProvider();

signals:
    void availableChanged();
    void runningChanged();
    void statusChanged();
    void framesChanged();
    void autoCaptureChanged();
    void previewChanged();
    void failed(const QString &message);

private:
    void startSession(const QString &projectPath);
    void setStatus(const QString &status);

    bool m_available = false;
    bool m_tracking = false;
    QString m_status;
    int m_frames = 0;
    int m_points = 0;
    bool m_autoCapture = true;
    int m_previewRevision = 0;
    ArCoreSession *m_session = nullptr;
};
