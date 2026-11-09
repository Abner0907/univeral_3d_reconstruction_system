#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QString>

// Platform capabilities exposed to QML as the singleton `AppInfo`.
class AppInfo : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool isMobile READ isMobile CONSTANT)
    Q_PROPERTY(bool canCapture READ canCapture CONSTANT)
    Q_PROPERTY(bool canReconstruct READ canReconstruct CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(int cpuThreads READ cpuThreads CONSTANT)

public:
    explicit AppInfo(QObject *parent = nullptr);

    bool isMobile() const;
    // Capturing and reconstructing happen on the phone. On desktop (the viewer) they can be
    // enabled for development with the environment variable U3D_DEV_RECONSTRUCT=1.
    bool canCapture() const;
    bool canReconstruct() const;
    QString version() const;
    int cpuThreads() const;
};
