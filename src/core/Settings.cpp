#include "Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>

namespace u3d {

namespace {

QString envString(const char *name)
{
    return qEnvironmentVariableIsSet(name) ? qEnvironmentVariable(name) : QString();
}

bool envBool(const char *name, bool fallback)
{
    if (!qEnvironmentVariableIsSet(name))
        return fallback;
    const QString v = qEnvironmentVariable(name).trimmed().toLower();
    return v == QLatin1String("1") || v == QLatin1String("true") || v == QLatin1String("yes")
           || v == QLatin1String("on");
}

int envInt(const char *name, int fallback)
{
    bool ok = false;
    const int v = qEnvironmentVariableIntValue(name, &ok);
    return ok ? v : fallback;
}

} // namespace

Settings Settings::defaults()
{
    Settings s;
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.u3d");
    s.dataDir = QDir(base).filePath(QStringLiteral("data"));
    return s;
}

Settings Settings::load(QSettings &store)
{
    Settings s = defaults();
    store.beginGroup(QStringLiteral("reconstruction"));
    s.dataDir = store.value(QStringLiteral("dataDir"), s.dataDir).toString();
    s.nsBinDir = store.value(QStringLiteral("nsBinDir"), s.nsBinDir).toString();
    s.dryRun = store.value(QStringLiteral("dryRun"), s.dryRun).toBool();
    s.vis = store.value(QStringLiteral("vis"), s.vis).toString();
    s.viewerPort = store.value(QStringLiteral("viewerPort"), s.viewerPort).toInt();
    s.defaultMethod = store.value(QStringLiteral("defaultMethod"), s.defaultMethod).toString();
    s.defaultIterations = store.value(QStringLiteral("defaultIterations"), s.defaultIterations).toInt();
    s.videoFramesTarget = store.value(QStringLiteral("videoFramesTarget"), s.videoFramesTarget).toInt();
    s.matchingMethod = store.value(QStringLiteral("matchingMethod"), s.matchingMethod).toString();
    s.serverPort = store.value(QStringLiteral("serverPort"), s.serverPort).toInt();
    store.endGroup();
    return s;
}

void Settings::save(QSettings &store) const
{
    store.beginGroup(QStringLiteral("reconstruction"));
    store.setValue(QStringLiteral("dataDir"), dataDir);
    store.setValue(QStringLiteral("nsBinDir"), nsBinDir);
    store.setValue(QStringLiteral("dryRun"), dryRun);
    store.setValue(QStringLiteral("vis"), vis);
    store.setValue(QStringLiteral("viewerPort"), viewerPort);
    store.setValue(QStringLiteral("defaultMethod"), defaultMethod);
    store.setValue(QStringLiteral("defaultIterations"), defaultIterations);
    store.setValue(QStringLiteral("videoFramesTarget"), videoFramesTarget);
    store.setValue(QStringLiteral("matchingMethod"), matchingMethod);
    store.setValue(QStringLiteral("serverPort"), serverPort);
    store.endGroup();
}

void Settings::applyEnvironment()
{
    if (const QString v = envString("U3D_DATA_DIR"); !v.isEmpty())
        dataDir = v;
    if (const QString v = envString("U3D_NS_BIN_DIR"); !v.isEmpty())
        nsBinDir = v;
    if (const QString v = envString("U3D_FAKE_NS"); !v.isEmpty())
        fakeNsPath = v;
    if (const QString v = envString("U3D_NS_VIS"); !v.isEmpty())
        vis = v;
    if (const QString v = envString("U3D_MATCHING_METHOD"); !v.isEmpty())
        matchingMethod = v;
    if (const QString v = envString("U3D_DEFAULT_METHOD"); !v.isEmpty())
        defaultMethod = v;
    dryRun = envBool("U3D_DRY_RUN", dryRun);
    viewerPort = envInt("U3D_VIEWER_PORT", viewerPort);
    defaultIterations = envInt("U3D_DEFAULT_ITERATIONS", defaultIterations);
    videoFramesTarget = envInt("U3D_VIDEO_FRAMES", videoFramesTarget);
    serverPort = envInt("U3D_PORT", serverPort);
}

QString Settings::resolvedFakeNsPath() const
{
    if (!fakeNsPath.isEmpty())
        return fakeNsPath;
#ifdef Q_OS_WIN
    const QString exe = QStringLiteral("u3d_fake_ns.exe");
#else
    const QString exe = QStringLiteral("u3d_fake_ns");
#endif
    QDir dir(QCoreApplication::applicationDirPath());
#ifdef Q_OS_MACOS
    // Inside an .app bundle the helper may sit next to the bundle.
    if (!dir.exists(exe) && dir.path().endsWith(QLatin1String(".app/Contents/MacOS"))) {
        QDir outer(dir);
        outer.cd(QStringLiteral("../../.."));
        if (outer.exists(exe))
            return outer.filePath(exe);
    }
#endif
    return dir.filePath(exe);
}

} // namespace u3d
