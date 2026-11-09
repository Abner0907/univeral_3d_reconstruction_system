// u3d_render_check - renders a .ply with the app's SplatView into an offscreen OpenGL texture
// (QQuickRenderControl, no window) and saves a PNG. Development tool for checking the viewer.
//
//   u3d_render_check input.ply output.png [width height] [yaw pitch]

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
    if (argc > 6) {
        view.setYaw(float(std::atof(argv[5])));
        view.setPitch(float(std::atof(argv[6])));
    }
    std::fprintf(stderr, "%d splats\n", view.splatCount());

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
