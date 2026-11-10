// u3d_render_check - renders a .ply with the app's SplatView into an offscreen OpenGL texture
// (QQuickRenderControl, no window) and saves a PNG. Development tool for checking the viewer.
//
//   u3d_render_check input.ply output.png [width height] [yaw pitch] [commands...]
//
// Commands (applied in order after loading; yaw/pitch "-" keeps the default view):
//   crop [cropped.ply]   enable cropping with the automatic box (wireframe); optionally save it
//   preset N             view preset (0 front, 1 side, 2 top, 3 back)
//   focus X Y            make the splat at item position (X, Y) the orbit centre (double tap)

#include "SplatView.h"

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QQuickGraphicsDevice>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QUrl>

#include <cstdio>

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s input.ply output.png [width height] [yaw pitch]\n", argv[0]);
        return 2;
    }
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);
    QGuiApplication app(argc, argv);
    const int w = argc > 4 ? std::atoi(argv[3]) : 960, h = argc > 4 ? std::atoi(argv[4]) : 720;

    QOpenGLContext context;
    context.setFormat(format);
    if (!context.create()) {
        std::fprintf(stderr, "no OpenGL context\n");
        return 1;
    }
    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    context.makeCurrent(&surface);
    std::fprintf(stderr, "OpenGL: %s\n", reinterpret_cast<const char *>(context.functions()->glGetString(GL_RENDERER)));

    GLuint texture = 0;
    QOpenGLFunctions *gl = context.functions();
    gl->glGenTextures(1, &texture);
    gl->glBindTexture(GL_TEXTURE_2D, texture);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    QQuickRenderControl control;
    QQuickWindow window(&control);
    window.setGraphicsDevice(QQuickGraphicsDevice::fromOpenGLContext(&context));
    window.setColor(QColor(30, 36, 46));
    window.resize(w, h);
    if (!control.initialize()) {
        std::fprintf(stderr, "QQuickRenderControl::initialize failed\n");
        return 1;
    }
    window.setRenderTarget(QQuickRenderTarget::fromOpenGLTexture(texture, QSize(w, h)));

    SplatView view(window.contentItem());
    view.setSize(QSizeF(w, h));
    QSignalSpy loaded(&view, &SplatView::loaded);
    view.setSource(QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])));
    if (!loaded.wait(60000) || !view.error().isEmpty()) {
        std::fprintf(stderr, "load failed: %s\n", qPrintable(view.error()));
        return 1;
    }
    if (argc > 6 && QString::fromLocal8Bit(argv[5]) != QLatin1String("-")) {
        view.setYaw(float(std::atof(argv[5])));
        view.setPitch(float(std::atof(argv[6])));
    }
    std::fprintf(stderr, "%d splats, orbit centre (%.2f %.2f %.2f), distance %.2f\n", view.splatCount(),
                 view.target().x(), view.target().y(), view.target().z(), view.distance());
    for (int i = 7; i < argc; ++i) {
        const QString cmd = QString::fromLocal8Bit(argv[i]);
        if (cmd == QLatin1String("crop")) {
            view.setCropEnabled(true);
            view.suggestCrop();
            std::fprintf(stderr, "crop box [%.2f %.2f %.2f] - [%.2f %.2f %.2f], keeps %d of %d\n", view.cropMin().x(),
                         view.cropMin().y(), view.cropMin().z(), view.cropMax().x(), view.cropMax().y(),
                         view.cropMax().z(), view.keptCount(), view.splatCount());
            if (i + 1 < argc && QString::fromLocal8Bit(argv[i + 1]).endsWith(QLatin1String(".ply"))) {
                QSignalSpy saved(&view, &SplatView::cropSaved);
                view.saveCropped(QUrl::fromLocalFile(QString::fromLocal8Bit(argv[++i])));
                if (!saved.wait(60000) || !saved.first().at(0).toBool()) {
                    std::fprintf(stderr, "saving the crop failed\n");
                    return 1;
                }
                std::fprintf(stderr, "saved %d Gaussians to %s\n", saved.first().at(1).toInt(), argv[i]);
            }
        } else if (cmd == QLatin1String("preset") && i + 1 < argc) {
            view.viewPreset(std::atoi(argv[++i]));
        } else if (cmd == QLatin1String("focus") && i + 2 < argc) {
            const double x = std::atof(argv[i + 1]), y = std::atof(argv[i + 2]);
            i += 2;
            const bool ok = view.focusAt(x, y);
            std::fprintf(stderr, "focus at (%.0f, %.0f): %s, orbit centre (%.2f %.2f %.2f)\n", x, y,
                         ok ? "hit" : "nothing there", view.target().x(), view.target().y(), view.target().z());
        }
    }

    for (int frame = 0; frame < 2; ++frame) {
        control.polishItems();
        control.beginFrame();
        control.sync();
        control.render();
        control.endFrame();
    }
    // Read the rendered texture back.
    context.makeCurrent(&surface);
    GLuint fbo = 0;
    gl->glGenFramebuffers(1, &fbo);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    QImage image(w, h, QImage::Format_RGBA8888);
    gl->glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
    gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    image = image.mirrored();
    if (!image.save(QString::fromLocal8Bit(argv[2]))) {
        std::fprintf(stderr, "could not save image\n");
        return 1;
    }
    return 0;
}
