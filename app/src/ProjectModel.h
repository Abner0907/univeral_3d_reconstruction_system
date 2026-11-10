#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QQmlEngine>
#include <QUrl>
#include <QVariantMap>

// Capture projects stored in the app's data folder (<AppData>/projects/<id>/, see
// engine/include/u3d/Project.h for the layout). project.json holds the display name.
class ProjectModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(QString rootDir READ rootDir CONSTANT)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        PathRole,
        ImageCountRole,
        HasResultRole,
        CreatedRole,
        SourceRole,
        ThumbnailRole,
        ResultUrlRole,
    };

    explicit ProjectModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString rootDir() const { return m_root; }

    Q_INVOKABLE void refresh();
    // source: "arcore", "camera" or "import". Returns the new project's folder.
    Q_INVOKABLE QString createProject(const QString &name, const QString &source);
    Q_INVOKABLE void renameProject(const QString &path, const QString &name);
    Q_INVOKABLE bool deleteProject(const QString &path);
    // Copies photos (file:// or Android content:// URLs) into the project; returns how many.
    Q_INVOKABLE int importImages(const QString &path, const QList<QUrl> &urls);
    Q_INVOKABLE bool exportResult(const QString &path, const QUrl &target);
    Q_INVOKABLE QString imagesDir(const QString &path) const;
    // Next free photo file name, e.g. <project>/images/IMG_0042.jpg
    Q_INVOKABLE QString nextImagePath(const QString &path) const;
    Q_INVOKABLE QVariantMap info(const QString &path) const;
    Q_INVOKABLE QUrl resultUrl(const QString &path) const;
    // Before the first crop: keep the uncropped model as output/splat_full.ply.
    Q_INVOKABLE bool backupResult(const QString &path);
    Q_INVOKABLE bool hasFullBackup(const QString &path) const;
    // Puts the uncropped model back as output/splat.ply.
    Q_INVOKABLE bool restoreFullResult(const QString &path);

signals:
    void countChanged();

private:
    struct Entry {
        QString path;
        QString name;
        QString source;
        QDateTime created;
        int imageCount = 0;
        bool hasResult = false;
        QString firstImage;
    };
    Entry readEntry(const QString &path) const;

    QString m_root;
    QList<Entry> m_entries;
};
