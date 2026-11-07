#pragma once

#include <QJsonObject>
#include <QMetaType>
#include <QString>

namespace u3d {

enum class JobStatus {
    Uploading, // created; inputs are still being added
    Queued,
    Running,
    Succeeded,
    Failed,
    Cancelled,
};

QString jobStatusToString(JobStatus status);
JobStatus jobStatusFromString(const QString &text);

struct Job {
    QString id;
    QString name;
    QString method;
    QString inputType; // "images" or "video" (known once the job is started)
    int numInputs = 0;
    int iterations = 0;
    JobStatus status = JobStatus::Uploading;
    QString stage = QStringLiteral("uploading");
    double progress = 0.0; // overall progress, 0..1
    QString message;
    QString resultFile; // relative to the job directory
    qint64 createdAt = 0; // ms since epoch
    qint64 updatedAt = 0;

    bool isFinished() const
    {
        return status == JobStatus::Succeeded || status == JobStatus::Failed
               || status == JobStatus::Cancelled;
    }
    bool hasResult() const { return status == JobStatus::Succeeded && !resultFile.isEmpty(); }

    QJsonObject toJson() const;
    static Job fromJson(const QJsonObject &obj);
};

} // namespace u3d

Q_DECLARE_METATYPE(u3d::Job)
