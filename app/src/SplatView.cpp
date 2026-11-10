#include "SplatView.h"

#include "u3d/splat/Crop.h"
#include "u3d/splat/SplatPly.h"

#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QQuickOpenGLUtils>
#include <QtConcurrent>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

const char *kVertexShader = R"(
layout(location = 0) in vec2 a_corner;
layout(location = 1) in vec3 a_center;
layout(location = 2) in vec4 a_color;
layout(location = 3) in vec3 a_covA; // xx xy xz
layout(location = 4) in vec3 a_covB; // yy yz zz
uniform mat4 u_view;
uniform mat4 u_proj;
uniform vec2 u_focal;    // pixels
uniform vec2 u_viewport; // pixels
uniform int u_cropEnabled;
uniform vec3 u_cropMin;
uniform vec3 u_cropMax;
uniform float u_minOpacity;
out vec4 v_color;
out vec2 v_pos;
void main() {
    if (u_cropEnabled != 0 && (any(lessThan(a_center, u_cropMin)) || any(greaterThan(a_center, u_cropMax))
                               || a_color.a < u_minOpacity)) {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0); // cropped away
        v_color = vec4(0.0);
        v_pos = vec2(0.0);
        return;
    }
    vec4 cam = u_view * vec4(a_center, 1.0);
    vec4 clip = u_proj * cam;
    float limit = 1.2 * clip.w;
    if (cam.z > -0.001 || clip.x < -limit || clip.x > limit || clip.y < -limit || clip.y > limit) {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
        v_color = vec4(0.0);
        v_pos = vec2(0.0);
        return;
    }
    mat3 S = mat3(a_covA.x, a_covA.y, a_covA.z,
                  a_covA.y, a_covB.x, a_covB.y,
                  a_covA.z, a_covB.y, a_covB.z);
    mat3 W = mat3(u_view);
    float d = -cam.z;
    // Jacobian of the perspective projection (view space: y up, looking down -z).
    vec3 j0 = vec3(u_focal.x / d, 0.0, u_focal.x * cam.x / (d * d));
    vec3 j1 = vec3(0.0, u_focal.y / d, u_focal.y * cam.y / (d * d));
    vec3 t0 = transpose(W) * j0;
    vec3 t1 = transpose(W) * j1;
    float a = dot(t0, S * t0) + 0.3;
    float b = dot(t0, S * t1);
    float c = dot(t1, S * t1) + 0.3;
    float mid = 0.5 * (a + c);
    float rad = length(vec2(0.5 * (a - c), b));
    float l1 = mid + rad;
    float l2 = max(mid - rad, 0.1);
    vec2 dir = (abs(b) < 1e-7 && a >= c) ? vec2(1.0, 0.0) : normalize(vec2(b, l1 - a));
    vec2 major = min(sqrt(2.0 * l1), 1024.0) * dir;
    vec2 minor = min(sqrt(2.0 * l2), 1024.0) * vec2(dir.y, -dir.x);
    vec2 ndc = clip.xy / clip.w;
    v_color = a_color;
    v_pos = a_corner;
    gl_Position = vec4(ndc + (a_corner.x * major + a_corner.y * minor) * 2.0 / u_viewport, 0.0, 1.0);
}
)";

const char *kFragmentShader = R"(
in vec4 v_color;
in vec2 v_pos;
out vec4 fragColor;
void main() {
    float A = -dot(v_pos, v_pos);
    if (A < -4.0)
        discard;
    float B = exp(A) * v_color.a;
    fragColor = vec4(B * v_color.rgb, B); // premultiplied, composited front to back
}
)";

// Crop-box wireframe.
const char *kLineVertexShader = R"(
layout(location = 0) in vec3 a_pos;
uniform mat4 u_mvp;
void main() { gl_Position = u_mvp * vec4(a_pos, 1.0); }
)";
const char *kLineFragmentShader = R"(
out vec4 fragColor;
void main() { fragColor = vec4(0.31, 0.76, 0.97, 1.0); }
)";

struct PackedSplat {
    float center[3];
    unsigned char color[4];
    float cov[6];
};
static_assert(sizeof(PackedSplat) == 40, "unexpected padding");

std::shared_ptr<SplatData> loadSplats(const QString &path, QString *error)
{
    u3d::GaussianModel m;
    std::string err;
    bool isSplat = true;
    if (!u3d::readSplatPly(path.toStdString(), m, &err, &isSplat)) {
        *error = QString::fromStdString(err);
        return nullptr;
    }
    auto d = std::make_shared<SplatData>();
    d->isSplat = isSplat;
    const std::size_t n = m.size();
    d->centers = m.means;
    d->colors.resize(n * 4);
    d->covariances.resize(n * 6);
    for (std::size_t i = 0; i < n; ++i) {
        for (int k = 0; k < 3; ++k) {
            const float c = std::clamp(0.5f + u3d::kShC0 * m.colors[i * 3 + k], 0.0f, 1.0f);
            d->colors[i * 4 + k] = static_cast<unsigned char>(std::lround(c * 255.0f));
        }
        d->colors[i * 4 + 3] = static_cast<unsigned char>(std::lround(u3d::sigmoid(m.opacities[i]) * 255.0f));
        const float *q = &m.quats[i * 4];
        const float qn = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]) + 1e-12f;
        const float w = q[0] / qn, x = q[1] / qn, y = q[2] / qn, z = q[3] / qn;
        const float R[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
                            2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                            2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)};
        float M[9];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                M[r * 3 + c] = R[r * 3 + c] * std::exp(m.scales[i * 3 + c]);
        auto sigma = [&](int r, int c) { return M[r * 3] * M[c * 3] + M[r * 3 + 1] * M[c * 3 + 1] + M[r * 3 + 2] * M[c * 3 + 2]; };
        float *cov = &d->covariances[i * 6];
        cov[0] = sigma(0, 0); cov[1] = sigma(0, 1); cov[2] = sigma(0, 2);
        cov[3] = sigma(1, 1); cov[4] = sigma(1, 2); cov[5] = sigma(2, 2);
    }
    // Robust framing: median centre and 80th-percentile distance (ignores floaters).
    if (n > 0) {
        float med[3];
        for (int k = 0; k < 3; ++k) {
            std::vector<float> axis(n);
            for (std::size_t i = 0; i < n; ++i)
                axis[i] = d->centers[i * 3 + k];
            std::nth_element(axis.begin(), axis.begin() + std::ptrdiff_t(n / 2), axis.end());
            med[k] = axis[n / 2];
        }
        d->center = QVector3D(med[0], med[1], med[2]);
        std::vector<float> dist(n);
        for (std::size_t i = 0; i < n; ++i)
            dist[i] = (QVector3D(d->centers[i * 3], d->centers[i * 3 + 1], d->centers[i * 3 + 2]) - d->center).length();
        std::nth_element(dist.begin(), dist.begin() + std::ptrdiff_t(n * 8 / 10), dist.end());
        d->radius = std::max(dist[n * 8 / 10], 1e-3f);
        const u3d::CropBox bounds = u3d::modelBounds(m);
        const u3d::CropBox suggested = u3d::suggestCropBox(m);
        d->boundsMin = QVector3D(bounds.min[0], bounds.min[1], bounds.min[2]);
        d->boundsMax = QVector3D(bounds.max[0], bounds.max[1], bounds.max[2]);
        d->suggestedMin = QVector3D(suggested.min[0], suggested.min[1], suggested.min[2]);
        d->suggestedMax = QVector3D(suggested.max[0], suggested.max[1], suggested.max[2]);
    }
    return d;
}

class SplatRenderer : public QQuickFramebufferObject::Renderer, protected QOpenGLExtraFunctions
{
public:
    SplatRenderer() = default;
    ~SplatRenderer() override
    {
        m_lineBuffer.destroy();
        m_lineVao.destroy();
        m_instances.destroy();
        m_corners.destroy();
        m_vao.destroy();
    }

    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override
    {
        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::NoAttachment);
        return new QOpenGLFramebufferObject(size, format);
    }

    void synchronize(QQuickFramebufferObject *item) override
    {
        auto *view = static_cast<SplatView *>(item);
        if (view->dataVersion() != m_dataVersion) {
            m_data = view->data();
            m_dataVersion = view->dataVersion();
            m_needsUpload = true;
        }
        const float yaw = qDegreesToRadians(view->yaw()), pitch = qDegreesToRadians(view->pitch());
        const QVector3D offset(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw));
        m_target = view->target();
        m_eye = m_target + offset * view->distance();
        m_distance = view->distance();
        m_cropEnabled = view->cropEnabled();
        m_cropMin = view->cropMin();
        m_cropMax = view->cropMax();
        m_minOpacity = view->minOpacity();
    }

    void render() override
    {
        if (!m_initialized)
            initialize();
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!m_data || m_data->size() == 0 || !m_program.isLinked()) {
            QQuickOpenGLUtils::resetOpenGLState();
            return;
        }

        const QSize size = framebufferObject()->size();
        QMatrix4x4 view;
        view.lookAt(m_eye, m_target, QVector3D(0, 1, 0));
        QMatrix4x4 proj;
        const float fovY = 50.0f;
        const float nearPlane = std::max(m_distance * 0.005f, 1e-4f);
        proj.perspective(fovY, float(size.width()) / float(std::max(1, size.height())), nearPlane, m_distance * 100.0f);
        const float focal = 0.5f * float(size.height()) / std::tan(qDegreesToRadians(fovY) * 0.5f);

        sortAndUpload(view);

        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE_MINUS_DST_ALPHA, GL_ONE, GL_ONE_MINUS_DST_ALPHA, GL_ONE);
        glViewport(0, 0, size.width(), size.height());
        m_program.bind();
        m_program.setUniformValue("u_view", view);
        m_program.setUniformValue("u_proj", proj);
        m_program.setUniformValue("u_focal", QVector2D(focal, focal));
        m_program.setUniformValue("u_viewport", QVector2D(float(size.width()), float(size.height())));
        m_program.setUniformValue("u_cropEnabled", m_cropEnabled ? 1 : 0);
        m_program.setUniformValue("u_cropMin", m_cropMin);
        m_program.setUniformValue("u_cropMax", m_cropMax);
        m_program.setUniformValue("u_minOpacity", m_minOpacity);
        m_vao.bind();
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, GLsizei(m_data->size()));
        m_vao.release();
        m_program.release();
        if (m_cropEnabled)
            drawCropBox(proj * view);
        QQuickOpenGLUtils::resetOpenGLState();
    }

private:
    void drawCropBox(const QMatrix4x4 &mvp)
    {
        const QVector3D a = m_cropMin, b = m_cropMax;
        const QVector3D c[8] = {{a.x(), a.y(), a.z()}, {b.x(), a.y(), a.z()}, {b.x(), b.y(), a.z()}, {a.x(), b.y(), a.z()},
                                {a.x(), a.y(), b.z()}, {b.x(), a.y(), b.z()}, {b.x(), b.y(), b.z()}, {a.x(), b.y(), b.z()}};
        const int edges[24] = {0, 1, 1, 2, 2, 3, 3, 0, 4, 5, 5, 6, 6, 7, 7, 4, 0, 4, 1, 5, 2, 6, 3, 7};
        float lines[24 * 3];
        for (int k = 0; k < 24; ++k) {
            lines[k * 3] = c[edges[k]].x();
            lines[k * 3 + 1] = c[edges[k]].y();
            lines[k * 3 + 2] = c[edges[k]].z();
        }
        glDisable(GL_BLEND);
        m_lineProgram.bind();
        m_lineProgram.setUniformValue("u_mvp", mvp);
        m_lineVao.bind();
        m_lineBuffer.bind();
        m_lineBuffer.allocate(lines, sizeof lines);
        glDrawArrays(GL_LINES, 0, 24);
        m_lineBuffer.release();
        m_lineVao.release();
        m_lineProgram.release();
    }

    void initialize()
    {
        initializeOpenGLFunctions();
        if (qEnvironmentVariableIsSet("U3D_DEBUG_GL"))
            std::fprintf(stderr, "SplatRenderer: %s | %s\n", reinterpret_cast<const char *>(glGetString(GL_VERSION)),
                         reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
        const bool es = QOpenGLContext::currentContext()->isOpenGLES();
        const QByteArray header = es ? QByteArrayLiteral("#version 300 es\nprecision highp float;\n")
                                     : QByteArrayLiteral("#version 330 core\n");
        m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, header + kVertexShader);
        m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, header + kFragmentShader);
        if (!m_program.link())
            qWarning("Splat shader: %s", qPrintable(m_program.log()));
        m_lineProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, header + kLineVertexShader);
        m_lineProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, header + kLineFragmentShader);
        if (!m_lineProgram.link())
            qWarning("Crop box shader: %s", qPrintable(m_lineProgram.log()));
        m_lineVao.create();
        m_lineVao.bind();
        m_lineBuffer.create();
        m_lineBuffer.setUsagePattern(QOpenGLBuffer::DynamicDraw);
        m_lineBuffer.bind();
        m_lineBuffer.allocate(24 * 3 * int(sizeof(float)));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        m_lineBuffer.release();
        m_lineVao.release();

        m_vao.create();
        m_vao.bind();
        const float corners[8] = {-2, -2, 2, -2, -2, 2, 2, 2};
        m_corners.create();
        m_corners.bind();
        m_corners.allocate(corners, sizeof corners);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        m_instances.create();
        m_instances.setUsagePattern(QOpenGLBuffer::DynamicDraw);
        m_instances.bind();
        const GLsizei stride = sizeof(PackedSplat);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(PackedSplat, center)));
        glVertexAttribDivisor(1, 1);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, reinterpret_cast<void *>(offsetof(PackedSplat, color)));
        glVertexAttribDivisor(2, 1);
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(PackedSplat, cov)));
        glVertexAttribDivisor(3, 1);
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(PackedSplat, cov) + 3 * sizeof(float)));
        glVertexAttribDivisor(4, 1);
        m_vao.release();
        m_initialized = true;
    }

    // Front-to-back order by view depth (16-bit counting sort), re-sorted only when the view
    // direction or position changed noticeably.
    void sortAndUpload(const QMatrix4x4 &view)
    {
        const std::size_t n = m_data->size();
        const QVector3D forward = (m_target - m_eye).normalized();
        const bool moved = m_needsUpload || QVector3D::dotProduct(forward, m_lastForward) < 0.9995f
                           || (m_eye - m_lastEye).length() > 0.02f * m_distance;
        if (!moved)
            return;
        m_lastForward = forward;
        m_lastEye = m_eye;
        m_needsUpload = false;

        const float *c = m_data->centers.data();
        m_depth.resize(n);
        float lo = std::numeric_limits<float>::max(), hi = -lo;
        const float r2 = view(2, 0), r2y = view(2, 1), r2z = view(2, 2), r2w = view(2, 3);
        for (std::size_t i = 0; i < n; ++i) {
            const float z = -(r2 * c[i * 3] + r2y * c[i * 3 + 1] + r2z * c[i * 3 + 2] + r2w); // distance in front
            m_depth[i] = z;
            lo = std::min(lo, z);
            hi = std::max(hi, z);
        }
        const float scale = hi > lo ? 65535.0f / (hi - lo) : 0.0f;
        std::vector<std::uint32_t> counts(65537, 0);
        m_keys.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            m_keys[i] = std::uint16_t((m_depth[i] - lo) * scale);
            ++counts[std::size_t(m_keys[i]) + 1];
        }
        for (std::size_t k = 1; k < counts.size(); ++k)
            counts[k] += counts[k - 1];
        m_order.resize(n);
        for (std::size_t i = 0; i < n; ++i)
            m_order[counts[m_keys[i]]++] = std::uint32_t(i);

        m_packed.resize(n);
        for (std::size_t k = 0; k < n; ++k) {
            const std::size_t i = m_order[k];
            PackedSplat &p = m_packed[k];
            std::memcpy(p.center, &c[i * 3], sizeof p.center);
            std::memcpy(p.color, &m_data->colors[i * 4], sizeof p.color);
            std::memcpy(p.cov, &m_data->covariances[i * 6], sizeof p.cov);
        }
        m_instances.bind();
        m_instances.allocate(m_packed.data(), int(n * sizeof(PackedSplat)));
        m_instances.release();
    }

    bool m_initialized = false;
    bool m_cropEnabled = false;
    QVector3D m_cropMin, m_cropMax;
    float m_minOpacity = 0.0f;
    QOpenGLShaderProgram m_lineProgram;
    QOpenGLVertexArrayObject m_lineVao;
    QOpenGLBuffer m_lineBuffer{QOpenGLBuffer::VertexBuffer};
    QOpenGLShaderProgram m_program;
    QOpenGLVertexArrayObject m_vao;
    QOpenGLBuffer m_corners{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_instances{QOpenGLBuffer::VertexBuffer};
    std::shared_ptr<const SplatData> m_data;
    int m_dataVersion = -1;
    bool m_needsUpload = false;
    QVector3D m_eye, m_target, m_lastEye, m_lastForward;
    float m_distance = 1.0f;
    std::vector<float> m_depth;
    std::vector<std::uint16_t> m_keys;
    std::vector<std::uint32_t> m_order;
    std::vector<PackedSplat> m_packed;
};

} // namespace

SplatView::SplatView(QQuickItem *parent)
    : QQuickFramebufferObject(parent)
{
    setMirrorVertically(true);
    connect(&m_watcher, &QFutureWatcher<std::shared_ptr<SplatData>>::finished, this, [this] {
        auto data = m_watcher.result();
        m_error = m_pendingError;
        m_data = data;
        ++m_dataVersion;
        m_loading = false;
        if (m_data) {
            m_cropMin = m_data->suggestedMin;
            m_cropMax = m_data->suggestedMax;
        }
        updateKeptCount();
        emit cropChanged();
        resetView();
        emit loadingChanged();
        emit loaded();
        update();
    });
}

QQuickFramebufferObject::Renderer *SplatView::createRenderer() const
{
    return new SplatRenderer();
}

void SplatView::setSource(const QUrl &url)
{
    if (url == m_source)
        return;
    m_source = url;
    emit sourceChanged();
    if (url.isEmpty())
        return;
    m_loading = true;
    emit loadingChanged();
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    m_watcher.setFuture(QtConcurrent::run([this, path] {
        QString error;
        auto d = loadSplats(path, &error);
        m_pendingError = error; // read on the GUI thread after finished()
        return d;
    }));
}

void SplatView::setYaw(float v)
{
    m_yaw = v;
    emit viewChanged();
    update();
}

void SplatView::setPitch(float v)
{
    m_pitch = std::clamp(v, -89.0f, 89.0f);
    emit viewChanged();
    update();
}

void SplatView::setDistance(float v)
{
    const float r = m_data ? m_data->radius : 1.0f;
    m_distance = std::clamp(v, r * 0.05f, r * 50.0f);
    emit viewChanged();
    update();
}

void SplatView::orbit(qreal dx, qreal dy)
{
    m_yaw -= float(dx) * 0.3f;
    setPitch(m_pitch + float(dy) * 0.3f);
}

void SplatView::pan(qreal dx, qreal dy)
{
    const float yaw = qDegreesToRadians(m_yaw), pitch = qDegreesToRadians(m_pitch);
    const QVector3D forward(-std::cos(pitch) * std::sin(yaw), -std::sin(pitch), -std::cos(pitch) * std::cos(yaw));
    const QVector3D right = QVector3D::crossProduct(forward, QVector3D(0, 1, 0)).normalized();
    const QVector3D up = QVector3D::crossProduct(right, forward);
    const float perPixel = 2.0f * m_distance * std::tan(qDegreesToRadians(25.0f)) / float(std::max(1.0, height()));
    m_target += (-right * float(dx) + up * float(dy)) * perPixel;
    emit viewChanged();
    update();
}

void SplatView::zoom(qreal factor)
{
    setDistance(m_distance / float(factor));
}

void SplatView::resetView()
{
    m_target = m_data ? m_data->center : QVector3D();
    m_distance = m_data ? m_data->radius * 2.2f : 3.0f;
    m_yaw = 0.0f;
    m_pitch = 15.0f;
    emit viewChanged();
    update();
}

// ----------------------------------------------------------------------------- cropping

void SplatView::updateKeptCount()
{
    int kept = 0;
    if (m_data) {
        const std::size_t n = m_data->size();
        const unsigned char minAlpha = static_cast<unsigned char>(std::lround(std::clamp(m_minOpacity, 0.0f, 1.0f) * 255.0f));
        for (std::size_t i = 0; i < n; ++i) {
            const float *c = &m_data->centers[i * 3];
            if (c[0] >= m_cropMin.x() && c[0] <= m_cropMax.x() && c[1] >= m_cropMin.y() && c[1] <= m_cropMax.y()
                && c[2] >= m_cropMin.z() && c[2] <= m_cropMax.z() && m_data->colors[i * 4 + 3] >= minAlpha)
                ++kept;
        }
    }
    m_keptCount = kept;
}

void SplatView::setCropEnabled(bool on)
{
    if (on == m_cropEnabled)
        return;
    m_cropEnabled = on;
    emit cropChanged();
    update();
}

void SplatView::setCropMin(const QVector3D &v)
{
    m_cropMin = v;
    updateKeptCount();
    emit cropChanged();
    update();
}

void SplatView::setCropMax(const QVector3D &v)
{
    m_cropMax = v;
    updateKeptCount();
    emit cropChanged();
    update();
}

void SplatView::setMinOpacity(float v)
{
    m_minOpacity = std::clamp(v, 0.0f, 1.0f);
    updateKeptCount();
    emit cropChanged();
    update();
}

void SplatView::suggestCrop()
{
    if (!m_data)
        return;
    m_cropMin = m_data->suggestedMin;
    m_cropMax = m_data->suggestedMax;
    updateKeptCount();
    emit cropChanged();
    update();
}

void SplatView::reload()
{
    const QUrl url = m_source;
    m_source.clear();
    setSource(url);
}

void SplatView::saveCropped(const QUrl &target)
{
    if (m_saving || m_source.isEmpty())
        return;
    m_saving = true;
    emit savingChanged();
    const std::string src = (m_source.isLocalFile() ? m_source.toLocalFile() : m_source.toString()).toStdString();
    const std::string dst = (target.isLocalFile() ? target.toLocalFile() : target.toString()).toStdString();
    u3d::CropBox box;
    for (int k = 0; k < 3; ++k) {
        box.min[k] = m_cropMin[k];
        box.max[k] = m_cropMax[k];
    }
    const float minOpacity = m_minOpacity;
    auto *watcher = new QFutureWatcher<QString>(this);
    auto count = std::make_shared<int>(0);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, count] {
        const QString error = watcher->result();
        watcher->deleteLater();
        m_saving = false;
        emit savingChanged();
        emit cropSaved(error.isEmpty(), *count, error);
    });
    watcher->setFuture(QtConcurrent::run([src, dst, box, minOpacity, count]() -> QString {
        u3d::GaussianModel model;
        std::string err;
        bool isSplat = true;
        if (!u3d::readSplatPly(src, model, &err, &isSplat))
            return QString::fromStdString(err);
        const u3d::GaussianModel cropped = u3d::cropModel(model, box, minOpacity);
        if (cropped.size() == 0)
            return QObject::tr("The crop box contains nothing.");
        if (!u3d::writeSplatPly(dst, cropped, &err))
            return QString::fromStdString(err);
        *count = int(cropped.size());
        return QString();
    }));
}
