#include "Pipeline.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace u3d::pipeline {

QString Command::toString() const
{
    QStringList parts{program};
    for (const QString &arg : arguments)
        parts << (arg.contains(QLatin1Char(' ')) ? QLatin1Char('"') + arg + QLatin1Char('"') : arg);
    return parts.join(QLatin1Char(' '));
}

QString stageName(Stage stage)
{
    switch (stage) {
    case Stage::Prepare: return QStringLiteral("prepare");
    case Stage::ProcessData: return QStringLiteral("process_data");
    case Stage::Train: return QStringLiteral("train");
    case Stage::Export: return QStringLiteral("export");
    }
    return {};
}

QString stageDescription(Stage stage, const QString &method)
{
    switch (stage) {
    case Stage::Prepare: return QStringLiteral("Preparing inputs");
    case Stage::ProcessData: return QStringLiteral("Estimating camera poses (COLMAP)");
    case Stage::Train: return QStringLiteral("Training %1").arg(method);
    case Stage::Export: return QStringLiteral("Exporting result");
    }
    return {};
}

QPair<double, double> stageRange(Stage stage)
{
    switch (stage) {
    case Stage::Prepare: return {0.0, 0.02};
    case Stage::ProcessData: return {0.02, 0.15};
    case Stage::Train: return {0.15, 0.95};
    case Stage::Export: return {0.95, 1.0};
    }
    return {0.0, 1.0};
}

QStringList splatMethods()
{
    return {QStringLiteral("splatfacto"), QStringLiteral("splatfacto-big")};
}

QStringList methods()
{
    return splatMethods() + QStringList{QStringLiteral("nerfacto"), QStringLiteral("nerfacto-big")};
}

bool isValidMethod(const QString &method) { return methods().contains(method); }
bool isSplatMethod(const QString &method) { return splatMethods().contains(method); }

bool isImageFile(const QString &fileName)
{
    static const QSet<QString> exts{QStringLiteral("jpg"), QStringLiteral("jpeg"),
                                    QStringLiteral("png"), QStringLiteral("tif"),
                                    QStringLiteral("tiff")};
    return exts.contains(QFileInfo(fileName).suffix().toLower());
}

bool isVideoFile(const QString &fileName)
{
    static const QSet<QString> exts{QStringLiteral("mp4"), QStringLiteral("mov"),
                                    QStringLiteral("avi"), QStringLiteral("mkv"),
                                    QStringLiteral("webm")};
    return exts.contains(QFileInfo(fileName).suffix().toLower());
}

QString classifyInputs(const QStringList &fileNames, QString *error)
{
    auto fail = [error](const QString &msg) {
        if (error)
            *error = msg;
        return QString();
    };
    if (fileNames.isEmpty())
        return fail(QStringLiteral("No input files were provided."));

    int images = 0, videos = 0;
    for (const QString &name : fileNames) {
        if (isImageFile(name))
            ++images;
        else if (isVideoFile(name))
            ++videos;
        else
            return fail(QStringLiteral("Unsupported file type: %1. Use JPG/PNG/TIFF images or "
                                       "one MP4/MOV video.")
                            .arg(QFileInfo(name).fileName()));
    }
    if (videos == 0) {
        if (images < 3)
            return fail(QStringLiteral("At least 3 images are required (30-200 recommended)."));
        return QStringLiteral("images");
    }
    if (images == 0 && videos == 1)
        return QStringLiteral("video");
    return fail(QStringLiteral("Provide either a set of images or exactly one video, not a mix."));
}

QString sanitizeFileName(const QString &name, const QString &fallback)
{
    static const QRegularExpression unsafe(QStringLiteral("[^A-Za-z0-9._-]+"));
    QString base = QFileInfo(QString(name).replace(QLatin1Char('\\'), QLatin1Char('/'))).fileName();
    base.replace(unsafe, QStringLiteral("_"));
    while (base.startsWith(QLatin1Char('.')) || base.startsWith(QLatin1Char('_')))
        base.remove(0, 1);
    return base.isEmpty() ? fallback : base.left(120);
}

QString stripAnsi(const QString &text)
{
    static const QRegularExpression ansi(QStringLiteral("\x1b\\[[0-9;?]*[ -/]*[@-~]"));
    QString out = text;
    out.remove(ansi);
    return out;
}

double parseTrainProgress(const QString &line)
{
    // ns-train prints a table whose first column looks like "12340 (41.13%)".
    static const QRegularExpression re(QStringLiteral("^\\s*(\\d+)\\s+\\((\\d+(?:\\.\\d+)?)%\\)"));
    const QRegularExpressionMatch m = re.match(stripAnsi(line));
    if (!m.hasMatch())
        return -1.0;
    return qBound(0.0, m.captured(2).toDouble() / 100.0, 1.0);
}

Command toolCommand(const Settings &settings, const QString &tool)
{
    if (settings.dryRun)
        return {settings.resolvedFakeNsPath(), {tool}};
    if (settings.nsBinDir.isEmpty())
        return {tool, {}};
    return {QDir(settings.nsBinDir).filePath(tool), {}};
}

Command processDataCommand(const Settings &settings, const JobPaths &paths,
                           const QString &inputType)
{
    Command cmd = toolCommand(settings, QStringLiteral("ns-process-data"));
    if (inputType == QLatin1String("video")) {
        const QFileInfoList files =
            QDir(paths.input()).entryInfoList(QDir::Files, QDir::Name);
        const QString video = files.isEmpty() ? paths.input() : files.first().absoluteFilePath();
        cmd.arguments << QStringLiteral("video")
                      << QStringLiteral("--data") << video
                      << QStringLiteral("--output-dir") << paths.processed()
                      << QStringLiteral("--num-frames-target")
                      << QString::number(settings.videoFramesTarget);
    } else {
        cmd.arguments << QStringLiteral("images")
                      << QStringLiteral("--data") << paths.input()
                      << QStringLiteral("--output-dir") << paths.processed();
    }
    cmd.arguments << QStringLiteral("--matching-method") << settings.matchingMethod;
    return cmd;
}

Command trainCommand(const Settings &settings, const JobPaths &paths, const QString &jobId,
                     const QString &method, int iterations)
{
    Command cmd = toolCommand(settings, QStringLiteral("ns-train"));
    cmd.arguments << method
                  << QStringLiteral("--data") << paths.processed()
                  << QStringLiteral("--output-dir") << paths.outputs()
                  << QStringLiteral("--experiment-name") << jobId
                  << QStringLiteral("--timestamp") << QStringLiteral("run")
                  << QStringLiteral("--max-num-iterations") << QString::number(iterations)
                  << QStringLiteral("--vis") << settings.vis;
    if (settings.vis.contains(QLatin1String("viewer"))) {
        cmd.arguments << QStringLiteral("--viewer.quit-on-train-completion") << QStringLiteral("True")
                      << QStringLiteral("--viewer.websocket-port")
                      << QString::number(settings.viewerPort);
    }
    return cmd;
}

Command exportCommand(const Settings &settings, const JobPaths &paths, const QString &method,
                      const QString &trainConfig)
{
    Command cmd = toolCommand(settings, QStringLiteral("ns-export"));
    if (isSplatMethod(method)) {
        cmd.arguments << QStringLiteral("gaussian-splat")
                      << QStringLiteral("--load-config") << trainConfig
                      << QStringLiteral("--output-dir") << paths.exports();
    } else {
        cmd.arguments << QStringLiteral("pointcloud")
                      << QStringLiteral("--load-config") << trainConfig
                      << QStringLiteral("--output-dir") << paths.exports()
                      << QStringLiteral("--num-points") << QStringLiteral("1000000")
                      << QStringLiteral("--remove-outliers") << QStringLiteral("True")
                      << QStringLiteral("--normal-method") << QStringLiteral("open3d");
    }
    return cmd;
}

QString findTrainConfig(const QString &outputsDir)
{
    QString newest;
    QDateTime newestTime;
    QDirIterator it(outputsDir, {QStringLiteral("config.yml")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QDateTime modified = QFileInfo(path).lastModified();
        if (newest.isEmpty() || modified > newestTime) {
            newest = path;
            newestTime = modified;
        }
    }
    return newest;
}

QString findExport(const QString &exportsDir)
{
    const QFileInfoList plys =
        QDir(exportsDir).entryInfoList({QStringLiteral("*.ply")}, QDir::Files, QDir::Name);
    return plys.isEmpty() ? QString() : plys.first().absoluteFilePath();
}

} // namespace u3d::pipeline
