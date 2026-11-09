#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QQmlEngine>
#include <QUrl>

#include <atomic>
#include <memory>

// Runs the on-device reconstruction (engine: u3d::reconstructProject) on a worker thread and
// publishes progress to QML.
class ReconstructionController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString stage READ stage NOTIFY progressChanged)
    Q_PROPERTY(QString message READ message NOTIFY progressChanged)
    Q_PROPERTY(QString log READ log NOTIFY logChanged)
    Q_PROPERTY(QString error READ error NOTIFY finished)
    Q_PROPERTY(QUrl resultUrl READ resultUrl NOTIFY finished)
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY runningChanged)

public:
    enum Quality { Fast = 0, Balanced = 1, High = 2 };
    Q_ENUM(Quality)

    explicit ReconstructionController(QObject *parent = nullptr);
    ~ReconstructionController() override;

    bool running() const { return m_running; }
    double progress() const { return m_progress; }
    QString stage() const { return m_stage; }
    QString message() const { return m_message; }
    QString log() const { return m_log; }
    QString error() const { return m_error; }
    QUrl resultUrl() const { return m_resultUrl; }
    QString projectPath() const { return m_projectPath; }

    // forceSfm: estimate camera poses from the photos even if ARCore poses were recorded.
    Q_INVOKABLE bool start(const QString &projectPath, int quality, bool forceSfm = false);
    Q_INVOKABLE void cancel();

signals:
    void runningChanged();
    void progressChanged();
    void logChanged();
    void finished(bool success);

private:
    void appendLog(const QString &line);
    void setKeepScreenOn(bool on);

    bool m_running = false;
    double m_progress = 0;
    QString m_stage;
    QString m_message;
    QString m_log;
    QString m_error;
    QUrl m_resultUrl;
    QString m_projectPath;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QFutureWatcher<QString> m_watcher; // result: error message, empty on success
};
