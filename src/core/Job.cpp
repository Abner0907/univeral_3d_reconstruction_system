#include "Job.h"

namespace u3d {

QString jobStatusToString(JobStatus status)
{
    switch (status) {
    case JobStatus::Uploading: return QStringLiteral("uploading");
    case JobStatus::Queued: return QStringLiteral("queued");
    case JobStatus::Running: return QStringLiteral("running");
    case JobStatus::Succeeded: return QStringLiteral("succeeded");
    case JobStatus::Failed: return QStringLiteral("failed");
    case JobStatus::Cancelled: return QStringLiteral("cancelled");
    }
    return QStringLiteral("failed");
}

JobStatus jobStatusFromString(const QString &text)
{
    if (text == QLatin1String("uploading")) return JobStatus::Uploading;
    if (text == QLatin1String("queued")) return JobStatus::Queued;
    if (text == QLatin1String("running")) return JobStatus::Running;
    if (text == QLatin1String("succeeded")) return JobStatus::Succeeded;
    if (text == QLatin1String("cancelled")) return JobStatus::Cancelled;
    return JobStatus::Failed;
}

QJsonObject Job::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), id);
    o.insert(QStringLiteral("name"), name);
    o.insert(QStringLiteral("method"), method);
    o.insert(QStringLiteral("input_type"), inputType);
    o.insert(QStringLiteral("num_inputs"), numInputs);
    o.insert(QStringLiteral("iterations"), iterations);
    o.insert(QStringLiteral("status"), jobStatusToString(status));
    o.insert(QStringLiteral("stage"), stage);
    o.insert(QStringLiteral("progress"), progress);
    o.insert(QStringLiteral("message"), message);
    o.insert(QStringLiteral("result_file"), resultFile);
    o.insert(QStringLiteral("has_result"), hasResult());
    o.insert(QStringLiteral("created_at"), double(createdAt));
    o.insert(QStringLiteral("updated_at"), double(updatedAt));
    return o;
}

Job Job::fromJson(const QJsonObject &o)
{
    Job j;
    j.id = o.value(QStringLiteral("id")).toString();
    j.name = o.value(QStringLiteral("name")).toString();
    j.method = o.value(QStringLiteral("method")).toString();
    j.inputType = o.value(QStringLiteral("input_type")).toString();
    j.numInputs = o.value(QStringLiteral("num_inputs")).toInt();
    j.iterations = o.value(QStringLiteral("iterations")).toInt();
    j.status = jobStatusFromString(o.value(QStringLiteral("status")).toString());
    j.stage = o.value(QStringLiteral("stage")).toString();
    j.progress = o.value(QStringLiteral("progress")).toDouble();
    j.message = o.value(QStringLiteral("message")).toString();
    j.resultFile = o.value(QStringLiteral("result_file")).toString();
    j.createdAt = qint64(o.value(QStringLiteral("created_at")).toDouble());
    j.updatedAt = qint64(o.value(QStringLiteral("updated_at")).toDouble());
    return j;
}

} // namespace u3d
