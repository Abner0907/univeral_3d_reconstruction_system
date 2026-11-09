#include "AppInfo.h"

#include <QThread>

AppInfo::AppInfo(QObject *parent)
    : QObject(parent)
{
}

bool AppInfo::isMobile() const
{
#ifdef Q_OS_ANDROID
    return true;
#else
    return false;
#endif
}

bool AppInfo::canCapture() const
{
    return isMobile() || qEnvironmentVariableIntValue("U3D_DEV_RECONSTRUCT") == 1;
}

bool AppInfo::canReconstruct() const
{
#ifdef U3D_HAS_RECONSTRUCTION
    return canCapture();
#else
    return false;
#endif
}

QString AppInfo::version() const
{
    return QStringLiteral(U3D_APP_VERSION);
}

int AppInfo::cpuThreads() const
{
    return QThread::idealThreadCount();
}
