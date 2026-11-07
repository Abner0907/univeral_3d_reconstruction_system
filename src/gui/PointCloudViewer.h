#pragma once

#include "PlyLoader.h"

#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QPoint>
#include <QVector3D>

// Interactive OpenGL preview of a reconstruction: point clouds and Gaussian splats
// (each splat drawn as a point in its base colour).
//   left drag: orbit   right/middle drag: pan   wheel: zoom   R: reset view
class PointCloudViewer : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT

public:
    enum class UpAxis { ZUp, YUp, NegYUp };

    explicit PointCloudViewer(QWidget *parent = nullptr);
    ~PointCloudViewer() override;

    void setPointCloud(const u3d::PointCloud &cloud);
    void clear();
    void setPointSize(float size);
    void setUpAxis(UpAxis axis);
    void resetView();
    std::size_t pointCount() const { return std::size_t(m_count); }

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    QMatrix4x4 rotation() const;
    QMatrix4x4 modelMatrix() const;
    void uploadPending();

    QOpenGLShaderProgram m_program;
    QOpenGLBuffer m_vbo{QOpenGLBuffer::VertexBuffer};
    std::vector<float> m_pending; // interleaved xyzrgb waiting for upload
    bool m_hasPending = false;
    int m_count = 0;

    QVector3D m_center;
    float m_radius = 1.0f;
    QVector3D m_target;
    float m_distance = 3.0f;
    float m_yaw = 30.0f;
    float m_pitch = 20.0f;
    float m_pointSize = 2.0f;
    UpAxis m_upAxis = UpAxis::ZUp;
    QPoint m_lastPos;
};
