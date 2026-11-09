#include "ArCoreSession.h"

#include "arcore_c_api.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QTextStream>

#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QtCore/qcoreapplication_platform.h>
#include <QJniEnvironment>
#include <QJniObject>
#endif

namespace {

// ---------------------------------------------------------------------------- ARCore via dlopen

#define U3D_ARCORE_FUNCTIONS(X)                                                                    \
    X(ArCoreApk_checkAvailability) X(ArCoreApk_requestInstall)                                     \
    X(ArSession_create) X(ArSession_destroy) X(ArSession_configure) X(ArSession_resume)            \
    X(ArSession_pause) X(ArSession_update) X(ArSession_setCameraTextureName)                       \
    X(ArSession_setDisplayGeometry) X(ArSession_setCameraConfig)                                   \
    X(ArSession_getSupportedCameraConfigsWithFilter)                                               \
    X(ArConfig_create) X(ArConfig_destroy) X(ArConfig_setFocusMode) X(ArConfig_setUpdateMode)      \
    X(ArCameraConfigFilter_create) X(ArCameraConfigFilter_destroy)                                 \
    X(ArCameraConfigList_create) X(ArCameraConfigList_destroy) X(ArCameraConfigList_getSize)       \
    X(ArCameraConfigList_getItem) X(ArCameraConfig_create) X(ArCameraConfig_destroy)               \
    X(ArCameraConfig_getImageDimensions)                                                           \
    X(ArFrame_create) X(ArFrame_destroy) X(ArFrame_acquireCamera) X(ArFrame_acquireCameraImage)    \
    X(ArFrame_acquirePointCloud)                                                                   \
    X(ArCamera_release) X(ArCamera_getTrackingState) X(ArCamera_getTrackingFailureReason)          \
    X(ArCamera_getPose) X(ArCamera_getImageIntrinsics)                                             \
    X(ArPose_create) X(ArPose_destroy) X(ArPose_getPoseRaw)                                        \
    X(ArCameraIntrinsics_create) X(ArCameraIntrinsics_destroy)                                     \
    X(ArCameraIntrinsics_getFocalLength) X(ArCameraIntrinsics_getPrincipalPoint)                   \
    X(ArCameraIntrinsics_getImageDimensions)                                                       \
    X(ArImage_release) X(ArImage_getWidth) X(ArImage_getHeight) X(ArImage_getFormat)              \
    X(ArImage_getPlaneData) X(ArImage_getPlaneRowStride) X(ArImage_getPlanePixelStride)            \
    X(ArPointCloud_release) X(ArPointCloud_getNumberOfPoints) X(ArPointCloud_getData)

struct Api {
#define U3D_DECLARE(name) decltype(&::name) name = nullptr;
    U3D_ARCORE_FUNCTIONS(U3D_DECLARE)
#undef U3D_DECLARE
    bool loaded = false;
};

Api *api()
{
    static Api a;
    static std::once_flag once;
    std::call_once(once, [] {
        void *handle = dlopen("libarcore_sdk_c.so", RTLD_NOW | RTLD_LOCAL);
        if (!handle)
            return;
#define U3D_LOAD(name)                                                       \
    a.name = reinterpret_cast<decltype(a.name)>(dlsym(handle, #name));      \
    if (!a.name)                                                             \
        return;
        U3D_ARCORE_FUNCTIONS(U3D_LOAD)
#undef U3D_LOAD
        a.loaded = true;
    });
    return a.loaded ? &a : nullptr;
}

void *jniEnv()
{
#ifdef Q_OS_ANDROID
    QJniEnvironment env;
    return env.jniEnv();
#else
    return nullptr;
#endif
}

void *activity()
{
#ifdef Q_OS_ANDROID
    return QNativeInterface::QAndroidApplication::context().object();
#else
    return nullptr;
#endif
}

// ---------------------------------------------------------------------------- helpers

QMutex g_previewMutex;
QImage g_preview;

constexpr unsigned kTextureExternalOes = 0x8D65; // GL_TEXTURE_EXTERNAL_OES

// YUV_420_888 camera image -> RGB, subsampled by `step`.
QImage toRgb(const Api &a, const ArSession *s, const ArImage *img, int step)
{
    int32_t w = 0, h = 0, format = 0;
    a.ArImage_getWidth(s, img, &w);
    a.ArImage_getHeight(s, img, &h);
    a.ArImage_getFormat(s, img, reinterpret_cast<ArImageFormat *>(&format));
    if (format != AR_IMAGE_FORMAT_YUV_420_888 || w <= 0 || h <= 0)
        return {};
    const uint8_t *py = nullptr, *pu = nullptr, *pv = nullptr;
    int32_t ly = 0, lu = 0, lv = 0, rowY = 0, rowUV = 0, pixUV = 0;
    a.ArImage_getPlaneData(s, img, 0, &py, &ly);
    a.ArImage_getPlaneData(s, img, 1, &pu, &lu);
    a.ArImage_getPlaneData(s, img, 2, &pv, &lv);
    a.ArImage_getPlaneRowStride(s, img, 0, &rowY);
    a.ArImage_getPlaneRowStride(s, img, 1, &rowUV);
    a.ArImage_getPlanePixelStride(s, img, 1, &pixUV);
    if (!py || !pu || !pv)
        return {};
    const int ow = w / step, oh = h / step;
    QImage out(ow, oh, QImage::Format_RGB888);
    for (int oy = 0; oy < oh; ++oy) {
        uchar *line = out.scanLine(oy);
        const int y = oy * step;
        for (int ox = 0; ox < ow; ++ox) {
            const int x = ox * step;
            const int iy = y * rowY + x;
            const int iuv = (y / 2) * rowUV + (x / 2) * pixUV;
            const float Y = iy < ly ? py[iy] : 0;
            const float U = (iuv < lu ? pu[iuv] : 128) - 128.0f;
            const float V = (iuv < lv ? pv[iuv] : 128) - 128.0f;
            line[ox * 3 + 0] = uchar(std::clamp(Y + 1.402f * V, 0.0f, 255.0f));
            line[ox * 3 + 1] = uchar(std::clamp(Y - 0.344136f * U - 0.714136f * V, 0.0f, 255.0f));
            line[ox * 3 + 2] = uchar(std::clamp(Y + 1.772f * U, 0.0f, 255.0f));
        }
    }
    return out;
}

// ARCore camera pose (camera-to-world, OpenGL camera axes relative to the image readout) ->
// world-to-camera in the OpenCV convention used by the reconstruction engine.
struct Pose {
    double R[9];   // world-to-camera
    double t[3];
    double C[3];   // camera centre
    double fwd[3]; // viewing direction in world
};

Pose poseFromRaw(const float raw[7])
{
    const double x = raw[0], y = raw[1], z = raw[2], w = raw[3];
    const double Rgl[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
                           2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                           2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)};
    // Flip the camera's y and z axes (OpenGL: y up, looking -z; OpenCV: y down, looking +z).
    double Rc2w[9];
    for (int r = 0; r < 3; ++r) {
        Rc2w[r * 3 + 0] = Rgl[r * 3 + 0];
        Rc2w[r * 3 + 1] = -Rgl[r * 3 + 1];
        Rc2w[r * 3 + 2] = -Rgl[r * 3 + 2];
    }
    Pose p;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            p.R[r * 3 + c] = Rc2w[c * 3 + r];
    p.C[0] = raw[4];
    p.C[1] = raw[5];
    p.C[2] = raw[6];
    for (int r = 0; r < 3; ++r) {
        p.t[r] = -(p.R[r * 3] * p.C[0] + p.R[r * 3 + 1] * p.C[1] + p.R[r * 3 + 2] * p.C[2]);
        p.fwd[r] = Rc2w[r * 3 + 2];
    }
    return p;
}

struct Intrinsics {
    float fx = 0, fy = 0, cx = 0, cy = 0;
    int width = 0, height = 0;
};

struct MapPoint {
    float xyz[3];
    float rgb[3] = {0, 0, 0};
    int colorSamples = 0;
};

QString trackingMessage(ArTrackingFailureReason reason)
{
    switch (reason) {
    case AR_TRACKING_FAILURE_REASON_INSUFFICIENT_LIGHT: return QObject::tr("Too dark - add light");
    case AR_TRACKING_FAILURE_REASON_EXCESSIVE_MOTION: return QObject::tr("Moving too fast - slow down");
    case AR_TRACKING_FAILURE_REASON_INSUFFICIENT_FEATURES: return QObject::tr("Not enough texture - point at detailed surfaces");
    case AR_TRACKING_FAILURE_REASON_CAMERA_UNAVAILABLE: return QObject::tr("Camera in use by another app");
    case AR_TRACKING_FAILURE_REASON_BAD_STATE: return QObject::tr("Tracking lost - restart the capture");
    default: return QObject::tr("Move the phone slowly to start tracking");
    }
}

} // namespace

// ---------------------------------------------------------------------------- availability

ArCoreSession::Availability ArCoreSession::checkAvailability()
{
    Api *a = api();
    if (!a)
        return Availability::Unsupported;
    ArAvailability availability = AR_AVAILABILITY_UNKNOWN_ERROR;
    a->ArCoreApk_checkAvailability(jniEnv(), activity(), &availability);
    switch (availability) {
    case AR_AVAILABILITY_SUPPORTED_INSTALLED: return Availability::Supported;
    case AR_AVAILABILITY_SUPPORTED_APK_TOO_OLD:
    case AR_AVAILABILITY_SUPPORTED_NOT_INSTALLED: return Availability::NeedsInstall;
    case AR_AVAILABILITY_UNKNOWN_CHECKING: return Availability::Checking;
    default: return Availability::Unsupported;
    }
}

bool ArCoreSession::requestInstall(QString *message)
{
    Api *a = api();
    if (!a) {
        *message = tr("ARCore is not available on this device.");
        return false;
    }
    ArInstallStatus status = AR_INSTALL_STATUS_INSTALLED;
    if (a->ArCoreApk_requestInstall(jniEnv(), activity(), 1, &status) != AR_SUCCESS) {
        *message = tr("Google Play Services for AR could not be installed.");
        return false;
    }
    if (status == AR_INSTALL_STATUS_INSTALL_REQUESTED) {
        *message = tr("Installing Google Play Services for AR - start the capture again afterwards.");
        return false;
    }
    return true;
}

QImage ArCoreSession::latestPreview()
{
    QMutexLocker lock(&g_previewMutex);
    return g_preview;
}

// ---------------------------------------------------------------------------- session

ArCoreSession::ArCoreSession(const QString &projectPath, QObject *parent)
    : QThread(parent)
    , m_projectPath(projectPath)
{
    // ArSession_update() needs a current GL context owning the camera texture.
    m_surface = new QOffscreenSurface();
    m_surface->setFormat(QSurfaceFormat::defaultFormat());
    m_surface->create();
    m_context = new QOpenGLContext();
    m_context->setFormat(QSurfaceFormat::defaultFormat());
    m_context->create();
    m_context->moveToThread(this);
}

ArCoreSession::~ArCoreSession()
{
    requestStop();
    wait();
    delete m_context;
    delete m_surface;
}

void ArCoreSession::run()
{
    Api *a = api();
    if (!a) {
        emit failed(tr("ARCore is not available on this device."));
        return;
    }
    if (!m_context->isValid() || !m_context->makeCurrent(m_surface)) {
        emit failed(tr("Could not create an OpenGL context for ARCore."));
        return;
    }

    ArSession *session = nullptr;
    if (a->ArSession_create(jniEnv(), activity(), &session) != AR_SUCCESS || !session) {
        m_context->doneCurrent();
        emit failed(tr("Could not start ARCore (is the camera permission granted?)."));
        return;
    }

    ArConfig *config = nullptr;
    a->ArConfig_create(session, &config);
    a->ArConfig_setFocusMode(session, config, AR_FOCUS_MODE_AUTO);
    a->ArConfig_setUpdateMode(session, config, AR_UPDATE_MODE_BLOCKING);
    a->ArSession_configure(session, config);
    a->ArConfig_destroy(config);

    // Largest CPU image up to 1920 px wide: the photos used for reconstruction.
    {
        ArCameraConfigFilter *filter = nullptr;
        ArCameraConfigList *list = nullptr;
        ArCameraConfig *cfg = nullptr;
        a->ArCameraConfigFilter_create(session, &filter);
        a->ArCameraConfigList_create(session, &list);
        a->ArSession_getSupportedCameraConfigsWithFilter(session, filter, list);
        a->ArCameraConfig_create(session, &cfg);
        int32_t count = 0, best = -1;
        long bestPixels = 0;
        a->ArCameraConfigList_getSize(session, list, &count);
        for (int32_t i = 0; i < count; ++i) {
            a->ArCameraConfigList_getItem(session, list, i, cfg);
            int32_t w = 0, h = 0;
            a->ArCameraConfig_getImageDimensions(session, cfg, &w, &h);
            if (w <= 1920 && long(w) * h > bestPixels) {
                bestPixels = long(w) * h;
                best = i;
            }
        }
        if (best >= 0) {
            a->ArCameraConfigList_getItem(session, list, best, cfg);
            a->ArSession_setCameraConfig(session, cfg);
        }
        a->ArCameraConfig_destroy(cfg);
        a->ArCameraConfigList_destroy(list);
        a->ArCameraConfigFilter_destroy(filter);
    }

    QOpenGLFunctions *gl = m_context->functions();
    GLuint texture = 0;
    gl->glGenTextures(1, &texture);
    gl->glBindTexture(kTextureExternalOes, texture);
    gl->glTexParameteri(kTextureExternalOes, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(kTextureExternalOes, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    a->ArSession_setCameraTextureName(session, texture);
    a->ArSession_setDisplayGeometry(session, 0, 1080, 1920);

    if (a->ArSession_resume(session) != AR_SUCCESS) {
        a->ArSession_destroy(session);
        gl->glDeleteTextures(1, &texture);
        m_context->doneCurrent();
        emit failed(tr("Could not open the camera for ARCore."));
        return;
    }

    ArFrame *frame = nullptr;
    a->ArFrame_create(session, &frame);
    ArPose *arPose = nullptr;
    a->ArPose_create(session, nullptr, &arPose);
    ArCameraIntrinsics *arIntrinsics = nullptr;
    a->ArCameraIntrinsics_create(session, &arIntrinsics);

    const QString imagesDir = QDir(m_projectPath).filePath(QStringLiteral("images"));
    const QString arDir = QDir(m_projectPath).filePath(QStringLiteral("arcore"));
    QDir().mkpath(imagesDir);
    QDir().mkpath(arDir);
    const QString camerasPath = QDir(arDir).filePath(QStringLiteral("cameras.txt"));
    int frames = int(QDir(imagesDir).entryList({QStringLiteral("*.jpg")}, QDir::Files).size());

    std::unordered_map<std::int64_t, MapPoint> points;
    bool tracking = false;
    QString lastMessage;
    bool haveKeyframe = false;
    Pose lastKeyframe{};
    QElapsedTimer sinceCapture;
    sinceCapture.start();
    int frameIndex = 0;

    while (!m_stop.load()) {
        if (a->ArSession_update(session, frame) != AR_SUCCESS) {
            msleep(10);
            continue;
        }
        ++frameIndex;
        ArCamera *camera = nullptr;
        a->ArFrame_acquireCamera(session, frame, &camera);
        ArTrackingState state = AR_TRACKING_STATE_STOPPED;
        a->ArCamera_getTrackingState(session, camera, &state);
        const bool nowTracking = state == AR_TRACKING_STATE_TRACKING;
        QString message;
        if (!nowTracking) {
            ArTrackingFailureReason reason = AR_TRACKING_FAILURE_REASON_NONE;
            a->ArCamera_getTrackingFailureReason(session, camera, &reason);
            message = trackingMessage(reason);
        } else {
            message = m_autoCapture.load() ? tr("Tracking - walk slowly around the object")
                                           : tr("Tracking - tap the shutter to take photos");
        }
        if (nowTracking != tracking || message != lastMessage) {
            tracking = nowTracking;
            lastMessage = message;
            emit trackingChanged(tracking, message);
        }

        Pose pose{};
        Intrinsics K;
        if (tracking) {
            float raw[7];
            a->ArCamera_getPose(session, camera, arPose);
            a->ArPose_getPoseRaw(session, arPose, raw);
            pose = poseFromRaw(raw);
            a->ArCamera_getImageIntrinsics(session, camera, arIntrinsics);
            a->ArCameraIntrinsics_getFocalLength(session, arIntrinsics, &K.fx, &K.fy);
            a->ArCameraIntrinsics_getPrincipalPoint(session, arIntrinsics, &K.cx, &K.cy);
            a->ArCameraIntrinsics_getImageDimensions(session, arIntrinsics, &K.width, &K.height);

            // Accumulate confident feature points on a 1 cm grid.
            if (frameIndex % 5 == 0 && points.size() < 200000) {
                ArPointCloud *cloud = nullptr;
                if (a->ArFrame_acquirePointCloud(session, frame, &cloud) == AR_SUCCESS && cloud) {
                    int32_t n = 0;
                    const float *data = nullptr;
                    a->ArPointCloud_getNumberOfPoints(session, cloud, &n);
                    a->ArPointCloud_getData(session, cloud, &data);
                    for (int32_t i = 0; data && i < n; ++i) {
                        const float *p = data + i * 4; // x y z confidence
                        if (p[3] < 0.3f)
                            continue;
                        const std::int64_t key = (std::int64_t(std::floor(p[0] * 100)) * 73856093)
                                                 ^ (std::int64_t(std::floor(p[1] * 100)) * 19349663)
                                                 ^ (std::int64_t(std::floor(p[2] * 100)) * 83492791);
                        auto it = points.find(key);
                        if (it == points.end()) {
                            MapPoint mp;
                            std::copy(p, p + 3, mp.xyz);
                            points.emplace(key, mp);
                        }
                    }
                    a->ArPointCloud_release(cloud);
                }
            }
        }

        // Keyframe decision.
        bool capture = m_captureRequested.exchange(false) && tracking;
        if (!capture && tracking && m_autoCapture.load() && sinceCapture.elapsed() > 300) {
            if (!haveKeyframe) {
                capture = true;
            } else {
                const double dx = pose.C[0] - lastKeyframe.C[0], dy = pose.C[1] - lastKeyframe.C[1],
                             dz = pose.C[2] - lastKeyframe.C[2];
                const double moved = std::sqrt(dx * dx + dy * dy + dz * dz);
                const double cosAngle = pose.fwd[0] * lastKeyframe.fwd[0] + pose.fwd[1] * lastKeyframe.fwd[1]
                                        + pose.fwd[2] * lastKeyframe.fwd[2];
                capture = moved > 0.07 || cosAngle < std::cos(7.0 * 3.14159265 / 180.0);
            }
        }

        const bool wantPreview = frameIndex % 3 == 0;
        if (capture || wantPreview) {
            ArImage *image = nullptr;
            if (a->ArFrame_acquireCameraImage(session, frame, &image) == AR_SUCCESS && image) {
                if (capture) {
                    const QImage full = toRgb(*a, session, image, 1);
                    QString name;
                    do {
                        name = QStringLiteral("IMG_%1.jpg").arg(frames++, 4, 10, QLatin1Char('0'));
                    } while (QFileInfo::exists(QDir(imagesDir).filePath(name)));
                    if (!full.isNull() && full.save(QDir(imagesDir).filePath(name), "JPG", 95)) {
                        // Intrinsics refer to the CPU image; rescale if it differs.
                        const double sx = K.width > 0 ? double(full.width()) / K.width : 1.0;
                        const double sy = K.height > 0 ? double(full.height()) / K.height : 1.0;
                        const bool isNew = !QFile::exists(camerasPath);
                        QFile file(camerasPath);
                        if (file.open(QIODevice::Append | QIODevice::Text)) {
                            QTextStream out(&file);
                            out.setRealNumberPrecision(10);
                            if (isNew)
                                out << "# name width height fx fy cx cy r00 r01 r02 r10 r11 r12 r20 r21 r22 tx ty tz\n";
                            out << name << ' ' << full.width() << ' ' << full.height() << ' ' << K.fx * sx << ' '
                                << K.fy * sy << ' ' << K.cx * sx << ' ' << K.cy * sy;
                            for (double r : pose.R)
                                out << ' ' << r;
                            for (double t : pose.t)
                                out << ' ' << t;
                            out << '\n';
                        }
                        // Colour the map points visible in this photo.
                        for (auto &kv : points) {
                            MapPoint &mp = kv.second;
                            const double X = mp.xyz[0], Y = mp.xyz[1], Z = mp.xyz[2];
                            const double pz = pose.R[6] * X + pose.R[7] * Y + pose.R[8] * Z + pose.t[2];
                            if (pz <= 0.05)
                                continue;
                            const double px = pose.R[0] * X + pose.R[1] * Y + pose.R[2] * Z + pose.t[0];
                            const double py = pose.R[3] * X + pose.R[4] * Y + pose.R[5] * Z + pose.t[1];
                            const int u = int(K.fx * sx * px / pz + K.cx * sx);
                            const int v = int(K.fy * sy * py / pz + K.cy * sy);
                            if (u < 0 || v < 0 || u >= full.width() || v >= full.height())
                                continue;
                            const QRgb c = full.pixel(u, v);
                            mp.rgb[0] += qRed(c) / 255.0f;
                            mp.rgb[1] += qGreen(c) / 255.0f;
                            mp.rgb[2] += qBlue(c) / 255.0f;
                            ++mp.colorSamples;
                        }
                        lastKeyframe = pose;
                        haveKeyframe = true;
                        sinceCapture.restart();
                        emit frameSaved(frames, int(points.size()));
                    }
                }
                if (wantPreview) {
                    // Small preview, rotated upright for a portrait screen.
                    QImage small = toRgb(*a, session, image, 4);
                    if (!small.isNull()) {
                        small = small.transformed(QTransform().rotate(90));
                        {
                            QMutexLocker lock(&g_previewMutex);
                            g_preview = small;
                        }
                        emit previewUpdated();
                    }
                }
                a->ArImage_release(image);
            }
        }
        a->ArCamera_release(camera);
    }

    // Save the sparse point cloud (x y z red green blue) for initialising the training.
    {
        QFile file(QDir(arDir).filePath(QStringLiteral("points.ply")));
        std::vector<const MapPoint *> list;
        for (const auto &kv : points)
            list.push_back(&kv.second);
        if (file.open(QIODevice::WriteOnly)) {
            QByteArray header = "ply\nformat binary_little_endian 1.0\nelement vertex "
                                + QByteArray::number(qsizetype(list.size()))
                                + "\nproperty float x\nproperty float y\nproperty float z\n"
                                  "property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n";
            file.write(header);
            for (const MapPoint *mp : list) {
                file.write(reinterpret_cast<const char *>(mp->xyz), sizeof mp->xyz); // Android is little endian
                for (int k = 0; k < 3; ++k) {
                    const float c = mp->colorSamples ? mp->rgb[k] / mp->colorSamples : 0.5f;
                    const uchar b = uchar(std::clamp(c, 0.0f, 1.0f) * 255.0f);
                    file.write(reinterpret_cast<const char *>(&b), 1);
                }
            }
        }
    }

    a->ArCameraIntrinsics_destroy(arIntrinsics);
    a->ArPose_destroy(arPose);
    a->ArFrame_destroy(frame);
    a->ArSession_pause(session);
    a->ArSession_destroy(session);
    gl->glDeleteTextures(1, &texture);
    m_context->doneCurrent();
}
