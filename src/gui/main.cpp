#include "ApiService.h"
#include "JobManager.h"
#include "MainWindow.h"
#include "Settings.h"

#include <QApplication>
#include <QSettings>
#include <QSurfaceFormat>

int main(int argc, char *argv[])
{
    // Depth buffer for the 3D viewer; must be set before the application object exists.
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setDepthBufferSize(24);
    format.setSamples(4);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("Universal3D"));
    QApplication::setApplicationName(QStringLiteral("U3DStudio"));
    QApplication::setApplicationDisplayName(QStringLiteral("U3D Studio"));
    QApplication::setApplicationVersion(QStringLiteral(U3D_VERSION));

    QSettings store;
    u3d::Settings settings = u3d::Settings::load(store);
    settings.applyEnvironment();

    u3d::JobManager jobs(settings);
    u3d::ApiService api(&jobs);
    MainWindow window(&jobs, &api);
    window.show();
    return app.exec();
}
