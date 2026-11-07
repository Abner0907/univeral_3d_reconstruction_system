#include "PointCloudViewer.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <random>

namespace {

// GLSL without #version works on desktop GL 2.1+/compatibility contexts and on GLES 2.
const char *kVertexShader = R"(
attribute highp vec3 a_position;
attribute lowp vec3 a_color;
uniform highp mat4 u_mvp;
uniform highp float u_pointSize;
varying lowp vec3 v_color;
void main() {
    gl_Position = u_mvp * vec4(a_position, 1.0);
    gl_PointSize = u_pointSize;
    v_color = a_color;
}
)";

const char *kFragmentShader = R"(
#ifdef GL_ES
precision mediump float;
#endif
varying lowp vec3 v_color;
void main() {
    gl_FragColor = vec4(v_color, 1.0);
}
)";

constexpr GLenum kProgramPointSize = 0x8642; // GL_PROGRAM_POINT_SIZE

QPoint eventPos(const QMouseEvent *e)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return e->position().toPoint();
#else
    return e->pos();
#endif
}

} // namespace

PointCloudViewer::PointCloudViewer(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 240);
}

PointCloudViewer::~PointCloudViewer()
{
    makeCurrent();
    m_vbo.destroy();
    doneCurrent();
}

void PointCloudViewer::setPointCloud(const u3d::PointCloud &cloud)
{
    const std::size_t n = cloud.size();
    m_pending.resize(n * 6);
    for (std::size_t i = 0; i < n; ++i) {
        std::copy_n(&cloud.positions[i * 3], 3, &m_pending[i * 6]);
        std::copy_n(&cloud.colors[i * 3], 3, &m_pending[i * 6 + 3]);
    }
    m_hasPending = true;

    // Robust framing: centre on the mean, radius = 90th percentile distance (ignores floaters).
    QVector3D sum;
    for (std::size_t i = 0; i < n; ++i)
        sum += QVector3D(cloud.positions[i * 3], cloud.positions[i * 3 + 1], cloud.positions[i * 3 + 2]);
    m_center = n ? sum / float(n) : QVector3D();
    std::vector<float> dist;
    const std::size_t samples = std::min<std::size_t>(n, 100000);
    dist.reserve(samples);
    std::mt19937 rng(42);
    std::uniform_int_distribution<std::size_t> pick(0, n ? n - 1 : 0);
    for (std::size_t s = 0; s < samples; ++s) {
        const std::size_t i = samples == n ? s : pick(rng);
        dist.push_back((QVector3D(cloud.positions[i * 3], cloud.positions[i * 3 + 1],
                                  cloud.positions[i * 3 + 2]) - m_center).length());
    }
    if (!dist.empty()) {
        auto nth = dist.begin() + std::ptrdiff_t(dist.size() * 9 / 10);
        std::nth_element(dist.begin(), nth, dist.end());
        m_radius = std::max(*nth, 1e-3f);
    } else {
        m_radius = 1.0f;
    }
    resetView();
}

void PointCloudViewer::clear()
{
    m_pending.clear();
    m_hasPending = true;
    update();
}

void PointCloudViewer::setPointSize(float size)
{
    m_pointSize = size;
    update();
}

void PointCloudViewer::setUpAxis(UpAxis axis)
{
    m_upAxis = axis;
    update();
}

void PointCloudViewer::resetView()
{
    m_target = m_center;
    m_distance = m_radius * 2.5f;
    m_yaw = 30.0f;
    m_pitch = 20.0f;
    update();
}

void PointCloudViewer::initializeGL()
{
    initializeOpenGLFunctions();
    m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader);
    m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader);
    m_program.bindAttributeLocation("a_position", 0);
    m_program.bindAttributeLocation("a_color", 1);
    if (!m_program.link())
        qWarning("Point shader failed to link: %s", qPrintable(m_program.log()));
    m_vbo.create();
    glEnable(GL_DEPTH_TEST);
    if (!context()->isOpenGLES())
        glEnable(kProgramPointSize);
}

void PointCloudViewer::resizeGL(int, int) {}

void PointCloudViewer::uploadPending()
{
    if (!m_hasPending)
        return;
    m_vbo.bind();
    m_vbo.allocate(m_pending.empty() ? nullptr : m_pending.data(),
                   int(m_pending.size() * sizeof(float)));
    m_vbo.release();
    m_count = int(m_pending.size() / 6);
    m_pending.clear();
    m_pending.shrink_to_fit();
    m_hasPending = false;
}

QMatrix4x4 PointCloudViewer::rotation() const
{
    QMatrix4x4 r;
    r.rotate(m_pitch, 1, 0, 0);
    r.rotate(m_yaw, 0, 1, 0);
    return r;
}

QMatrix4x4 PointCloudViewer::modelMatrix() const
{
    // Converts the data's up axis to the viewer's +Y.
    QMatrix4x4 m;
    switch (m_upAxis) {
    case UpAxis::ZUp: m.rotate(-90.0f, 1, 0, 0); break; // Nerfstudio world
    case UpAxis::NegYUp: m.rotate(180.0f, 1, 0, 0); break; // COLMAP / OpenCV
    case UpAxis::YUp: break;
    }
    return m;
}

void PointCloudViewer::paintGL()
{
    uploadPending();
    glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (m_count == 0 || !m_program.isLinked())
        return;

    const float aspect = float(width()) / float(std::max(1, height()));
    QMatrix4x4 projection;
    const float nearPlane = std::max(m_distance * 0.01f, m_radius * 0.0005f);
    projection.perspective(45.0f, aspect, nearPlane, m_distance + m_radius * 20.0f);

    QMatrix4x4 view;
    view.translate(0, 0, -m_distance);
    view *= rotation();
    const QMatrix4x4 model = modelMatrix();
    view.translate(-(model.map(m_target)));

    m_program.bind();
    m_program.setUniformValue("u_mvp", projection * view * model);
    m_program.setUniformValue("u_pointSize", m_pointSize * float(devicePixelRatioF()));
    m_vbo.bind();
    m_program.enableAttributeArray(0);
    m_program.enableAttributeArray(1);
    m_program.setAttributeBuffer(0, GL_FLOAT, 0, 3, 6 * sizeof(float));
    m_program.setAttributeBuffer(1, GL_FLOAT, 3 * sizeof(float), 3, 6 * sizeof(float));
    glDrawArrays(GL_POINTS, 0, m_count);
    m_program.disableAttributeArray(0);
    m_program.disableAttributeArray(1);
    m_vbo.release();
    m_program.release();
}

void PointCloudViewer::mousePressEvent(QMouseEvent *event)
{
    m_lastPos = eventPos(event);
}

void PointCloudViewer::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint pos = eventPos(event);
    const QPoint delta = pos - m_lastPos;
    m_lastPos = pos;
    if (event->buttons() & Qt::LeftButton) {
        m_yaw += delta.x() * 0.4f;
        m_pitch = std::clamp(m_pitch + delta.y() * 0.4f, -89.0f, 89.0f);
    } else if (event->buttons() & (Qt::RightButton | Qt::MiddleButton)) {
        // Move the orbit target in the camera plane, scaled so the scene follows the cursor.
        const float perPixel = 2.0f * m_distance * std::tan(qDegreesToRadians(22.5f)) / float(std::max(1, height()));
        const QMatrix4x4 inv = (rotation() * modelMatrix()).inverted();
        const QVector3D right = inv.mapVector(QVector3D(1, 0, 0));
        const QVector3D up = inv.mapVector(QVector3D(0, 1, 0));
        m_target += (-right * float(delta.x()) + up * float(delta.y())) * perPixel;
    } else {
        return;
    }
    update();
}

void PointCloudViewer::wheelEvent(QWheelEvent *event)
{
    const float steps = event->angleDelta().y() / 120.0f;
    m_distance = std::clamp(m_distance * std::pow(0.85f, steps), m_radius * 0.01f, m_radius * 100.0f);
    update();
    event->accept();
}

void PointCloudViewer::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_R)
        resetView();
    else
        QOpenGLWidget::keyPressEvent(event);
}
