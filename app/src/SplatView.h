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
    QVector3D boundsMin, boundsMax;      // range of the crop sliders (bulk of the model)
    QVector3D suggestedMin, suggestedMax; // automatic crop box around the main object
    QVector3D objectCenter;              // centre of the main object (orbit centre)
    float objectRadius = 1.0f;           // its radius (initial framing, zoom limits)
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

    // Cropping: splats outside [cropMin, cropMax] or fainter than minOpacity are hidden while
    // cropEnabled, and removed by saveCropped().
    Q_PROPERTY(bool cropEnabled READ cropEnabled WRITE setCropEnabled NOTIFY cropChanged)
    Q_PROPERTY(QVector3D cropMin READ cropMin WRITE setCropMin NOTIFY cropChanged)
    Q_PROPERTY(QVector3D cropMax READ cropMax WRITE setCropMax NOTIFY cropChanged)
    Q_PROPERTY(float minOpacity READ minOpacity WRITE setMinOpacity NOTIFY cropChanged)
    Q_PROPERTY(int keptCount READ keptCount NOTIFY cropChanged)
    Q_PROPERTY(QVector3D boundsMin READ boundsMin NOTIFY loaded)
    Q_PROPERTY(QVector3D boundsMax READ boundsMax NOTIFY loaded)
    Q_PROPERTY(bool saving READ saving NOTIFY savingChanged)

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

    bool cropEnabled() const { return m_cropEnabled; }
    QVector3D cropMin() const { return m_cropMin; }
    QVector3D cropMax() const { return m_cropMax; }
    float minOpacity() const { return m_minOpacity; }
    int keptCount() const { return m_keptCount; }
    QVector3D boundsMin() const { return m_data ? m_data->boundsMin : QVector3D(-1, -1, -1); }
    QVector3D boundsMax() const { return m_data ? m_data->boundsMax : QVector3D(1, 1, 1); }
    bool saving() const { return m_saving; }
    void setCropEnabled(bool on);
    void setCropMin(const QVector3D &v);
    void setCropMax(const QVector3D &v);
    void setMinOpacity(float v);

    // Sets the crop box around the main object (ignoring background and floaters).
    Q_INVOKABLE void suggestCrop();
    // Writes the cropped model (re-read from `source` at full precision) to `target`; emits
    // cropSaved when done. Saving over the source file is allowed.
    Q_INVOKABLE void saveCropped(const QUrl &target);
    // Reloads `source` from disk (e.g. after it was overwritten by saveCropped).
    Q_INVOKABLE void reload();

    // Gestures (pixels of finger/mouse movement).
    Q_INVOKABLE void orbit(qreal dx, qreal dy);
    Q_INVOKABLE void pan(qreal dx, qreal dy);
    Q_INVOKABLE void zoom(qreal factor);
    // Frames the main object from the front (orbit centre = object centre).
    Q_INVOKABLE void resetView();
    // Moves the orbit centre back to the object (or the crop box) without changing the angle.
    Q_INVOKABLE void centerOnObject();
    // 0 = front, 1 = side, 2 = top, 3 = back.
    Q_INVOKABLE void viewPreset(int preset);
    // Makes the splat under item position (x, y) the orbit centre. Returns false if nothing
    // is there.
    Q_INVOKABLE bool focusAt(qreal x, qreal y);

    // Renderer access.
    std::shared_ptr<const SplatData> data() const { return m_data; }
    int dataVersion() const { return m_dataVersion; }
    QVector3D target() const { return m_target; }
    // Camera shared by the renderer and picking.
    QVector3D eye() const;
    static constexpr float kFovY = 50.0f;

signals:
    void sourceChanged();
    void loadingChanged();
    void loaded();
    void viewChanged();
    void cropChanged();
    void savingChanged();
    void cropSaved(bool ok, int count, const QString &error);

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
    void updateKeptCount();
    bool m_cropEnabled = false;
    QVector3D m_cropMin{-1, -1, -1};
    QVector3D m_cropMax{1, 1, 1};
    float m_minOpacity = 0.0f;
    int m_keptCount = 0;
    bool m_saving = false;
};
