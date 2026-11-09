#pragma once

#include <QFutureWatcher>
#include <QQmlEngine>
#include <QQuickFramebufferObject>
#include <QUrl>
#include <QVector3D>

#include <memory>
#include <vector>

// Render-ready Gaussian splats: centre, colour (rgba8, straight alpha), 3D covariance.
struct SplatData {
    std::vector<float> centers;          // 3 per splat
    std::vector<unsigned char> colors;   // 4 per splat
    std::vector<float> covariances;      // 6 per splat: xx xy xz yy yz zz
    QVector3D center;                    // robust scene centre
    float radius = 1.0f;                 // robust scene radius
    bool isSplat = true;                 // false: loaded from a plain point cloud
    std::size_t size() const { return centers.size() / 3; }
};

// QML item showing a 3D Gaussian Splatting .ply (or point cloud) with an orbit camera.
// Splats are drawn as depth-sorted, alpha-blended screen-space ellipses (EWA projection of
// each 3D covariance), like web/desktop splat viewers.
class SplatView : public QQuickFramebufferObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(int splatCount READ splatCount NOTIFY loaded)
    Q_PROPERTY(bool isSplat READ isSplat NOTIFY loaded)
    Q_PROPERTY(QString error READ error NOTIFY loaded)
    Q_PROPERTY(float yaw READ yaw WRITE setYaw NOTIFY viewChanged)
    Q_PROPERTY(float pitch READ pitch WRITE setPitch NOTIFY viewChanged)
    Q_PROPERTY(float distance READ distance WRITE setDistance NOTIFY viewChanged)

public:
    explicit SplatView(QQuickItem *parent = nullptr);
    Renderer *createRenderer() const override;

    QUrl source() const { return m_source; }
    void setSource(const QUrl &url);
    bool loading() const { return m_loading; }
    int splatCount() const { return m_data ? int(m_data->size()) : 0; }
    bool isSplat() const { return m_data ? m_data->isSplat : true; }
    QString error() const { return m_error; }

    float yaw() const { return m_yaw; }
    float pitch() const { return m_pitch; }
    float distance() const { return m_distance; }
    void setYaw(float v);
    void setPitch(float v);
    void setDistance(float v);

    // Gestures (pixels of finger/mouse movement).
    Q_INVOKABLE void orbit(qreal dx, qreal dy);
    Q_INVOKABLE void pan(qreal dx, qreal dy);
    Q_INVOKABLE void zoom(qreal factor);
    Q_INVOKABLE void resetView();

    // Renderer access.
    std::shared_ptr<const SplatData> data() const { return m_data; }
    int dataVersion() const { return m_dataVersion; }
    QVector3D target() const { return m_target; }

signals:
    void sourceChanged();
    void loadingChanged();
    void loaded();
    void viewChanged();

private:
    QUrl m_source;
    bool m_loading = false;
    QString m_error;
    std::shared_ptr<const SplatData> m_data;
    int m_dataVersion = 0;
    float m_yaw = 0.0f;
    float m_pitch = 15.0f;
    float m_distance = 3.0f;
    QVector3D m_target;
    QFutureWatcher<std::shared_ptr<SplatData>> m_watcher;
    QString m_pendingError;
};
