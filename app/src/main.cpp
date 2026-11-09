#include "ArCapture.h"

#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickImageProvider>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QTimer>

#include <cstdio>
#include <QUrl>

int main(int argc, char *argv[])
{
    // The splat viewer renders with OpenGL (ES 3.0 on Android, 3.3 core on desktop).
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
#ifdef Q_OS_ANDROID
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(3, 0);
#else
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
#endif
    QSurfaceFormat::setDefaultFormat(format);

    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("Universal3D"));
    QGuiApplication::setApplicationName(QStringLiteral("U3D"));
    QGuiApplication::setApplicationVersion(QStringLiteral(U3D_APP_VERSION));
    QQuickStyle::setStyle(QStringLiteral("Material"));

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("arcore"), ArCapture::createImageProvider());
    // Desktop: `U3D file.ply` opens the file in the viewer.
    const QStringList args = app.arguments();
    engine.rootContext()->setContextProperty(
        QStringLiteral("startupFile"),
        args.size() > 1 ? QUrl::fromUserInput(args.at(1), QDir::currentPath(), QUrl::AssumeLocalFile) : QUrl());
    // Development: U3D_DEV_PAGE=capture opens the capture page of a new scan at start-up.
    engine.rootContext()->setContextProperty(QStringLiteral("startupPage"), qEnvironmentVariable("U3D_DEV_PAGE"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("U3D", "Main");

    // Development hook: U3D_SCREENSHOT=out.png saves the window after a few seconds and quits
    // (used to check the UI and the splat renderer without a visible window).
    const QString screenshot = qEnvironmentVariable("U3D_SCREENSHOT");
    if (!screenshot.isEmpty() && !engine.rootObjects().isEmpty()) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QTimer::singleShot(qEnvironmentVariableIntValue("U3D_SCREENSHOT_DELAY_MS") > 0
                               ? qEnvironmentVariableIntValue("U3D_SCREENSHOT_DELAY_MS") : 4000,
                           &app, [window, screenshot] {
            if (window) {
                if (qEnvironmentVariableIsSet("U3D_DEBUG_GL"))
                    std::fprintf(stderr, "graphics api %d, renderer interface %d\n", int(QQuickWindow::graphicsApi()),
                                 window->rendererInterface() ? int(window->rendererInterface()->graphicsApi()) : -1);
                window->grabWindow().save(screenshot);
            }
            QCoreApplication::exit(0);
        });
    }
    return app.exec();
}
