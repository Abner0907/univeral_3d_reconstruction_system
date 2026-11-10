#include "ProjectModel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <algorithm>

namespace {

QStringList imageFiles(const QString &dir)
{
    return QDir(dir).entryList({QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.png"),
                                QStringLiteral("*.JPG"), QStringLiteral("*.JPEG"), QStringLiteral("*.PNG")},
                               QDir::Files, QDir::Name);
}

QString localOrContentPath(const QUrl &url)
{
    // Android's document picker returns content:// URLs, which QFile opens directly.
    return url.isLocalFile() ? url.toLocalFile() : url.toString();
}

} // namespace

ProjectModel::ProjectModel(QObject *parent)
    : QAbstractListModel(parent)
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.u3d");
    m_root = QDir(base).filePath(QStringLiteral("projects"));
    QDir().mkpath(m_root);
    refresh();
}

int ProjectModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_entries.size());
}

QHash<int, QByteArray> ProjectModel::roleNames() const
{
    return {{NameRole, "name"},           {PathRole, "path"},       {ImageCountRole, "imageCount"},
            {HasResultRole, "hasResult"}, {CreatedRole, "created"}, {SourceRole, "source"},
            {ThumbnailRole, "thumbnail"}, {ResultUrlRole, "resultUrl"}};
}

QVariant ProjectModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(index.row());
    switch (role) {
    case NameRole: return e.name;
    case PathRole: return e.path;
    case ImageCountRole: return e.imageCount;
    case HasResultRole: return e.hasResult;
    case CreatedRole: return QLocale().toString(e.created, QLocale::ShortFormat);
    case SourceRole: return e.source;
    case ThumbnailRole: return e.firstImage.isEmpty() ? QUrl() : QUrl::fromLocalFile(e.firstImage);
    case ResultUrlRole: return resultUrl(e.path);
    default: return {};
    }
}

ProjectModel::Entry ProjectModel::readEntry(const QString &path) const
{
    Entry e;
    e.path = path;
    QFile meta(QDir(path).filePath(QStringLiteral("project.json")));
    if (meta.open(QIODevice::ReadOnly)) {
        const QJsonObject o = QJsonDocument::fromJson(meta.readAll()).object();
        e.name = o.value(QStringLiteral("name")).toString();
        e.source = o.value(QStringLiteral("source")).toString();
        e.created = QDateTime::fromString(o.value(QStringLiteral("created")).toString(), Qt::ISODate);
    }
    if (e.name.isEmpty())
        e.name = QFileInfo(path).fileName();
    if (!e.created.isValid())
        e.created = QFileInfo(path).birthTime();
    const QString images = imagesDir(path);
    const QStringList files = imageFiles(images);
    e.imageCount = int(files.size());
    if (!files.isEmpty())
        e.firstImage = QDir(images).filePath(files.first());
    e.hasResult = QFileInfo::exists(QDir(path).filePath(QStringLiteral("output/splat.ply")));
    return e;
}

void ProjectModel::refresh()
{
    beginResetModel();
    m_entries.clear();
    const QFileInfoList dirs = QDir(m_root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &d : dirs)
        m_entries.append(readEntry(d.absoluteFilePath()));
    std::sort(m_entries.begin(), m_entries.end(), [](const Entry &a, const Entry &b) { return a.created > b.created; });
    endResetModel();
    emit countChanged();
}

QString ProjectModel::createProject(const QString &name, const QString &source)
{
    const QDateTime now = QDateTime::currentDateTime();
    QString id = now.toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString path = QDir(m_root).filePath(id);
    for (int n = 2; QFileInfo::exists(path); ++n)
        path = QDir(m_root).filePath(id + QLatin1Char('-') + QString::number(n));
    QDir().mkpath(QDir(path).filePath(QStringLiteral("images")));

    QJsonObject o;
    o.insert(QStringLiteral("name"), name.trimmed().isEmpty()
                                         ? QStringLiteral("Scan ") + QLocale().toString(now, QLocale::ShortFormat)
                                         : name.trimmed());
    o.insert(QStringLiteral("source"), source);
    o.insert(QStringLiteral("created"), now.toString(Qt::ISODate));
    QFile meta(QDir(path).filePath(QStringLiteral("project.json")));
    if (meta.open(QIODevice::WriteOnly))
        meta.write(QJsonDocument(o).toJson());
    refresh();
    return path;
}

void ProjectModel::renameProject(const QString &path, const QString &name)
{
    QFile meta(QDir(path).filePath(QStringLiteral("project.json")));
    QJsonObject o;
    if (meta.open(QIODevice::ReadOnly))
        o = QJsonDocument::fromJson(meta.readAll()).object();
    meta.close();
    o.insert(QStringLiteral("name"), name.trimmed());
    if (meta.open(QIODevice::WriteOnly))
        meta.write(QJsonDocument(o).toJson());
    refresh();
}

bool ProjectModel::deleteProject(const QString &path)
{
    // Only ever delete inside our own projects folder.
    if (!QFileInfo(path).absoluteFilePath().startsWith(QFileInfo(m_root).absoluteFilePath() + QLatin1Char('/')))
        return false;
    const bool ok = QDir(path).removeRecursively();
    refresh();
    return ok;
}

int ProjectModel::importImages(const QString &path, const QList<QUrl> &urls)
{
    int copied = 0;
    for (const QUrl &url : urls) {
        QFile src(localOrContentPath(url));
        if (!src.open(QIODevice::ReadOnly))
            continue;
        const QByteArray data = src.readAll();
        // Keep JPEG/PNG; the engine decodes both (EXIF orientation is applied when loading).
        const bool png = data.startsWith("\x89PNG");
        QString target = nextImagePath(path);
        if (png)
            target.replace(QStringLiteral(".jpg"), QStringLiteral(".png"));
        QFile out(target);
        if (out.open(QIODevice::WriteOnly) && out.write(data) == data.size())
            ++copied;
    }
    refresh();
    return copied;
}

bool ProjectModel::exportResult(const QString &path, const QUrl &target)
{
    QFile src(QDir(path).filePath(QStringLiteral("output/splat.ply")));
    if (!src.open(QIODevice::ReadOnly))
        return false;
    QFile dst(localOrContentPath(target));
    if (!dst.open(QIODevice::WriteOnly))
        return false;
    while (!src.atEnd())
        if (dst.write(src.read(1 << 20)) < 0)
            return false;
    return true;
}

QString ProjectModel::imagesDir(const QString &path) const
{
    return QDir(path).filePath(QStringLiteral("images"));
}

QString ProjectModel::nextImagePath(const QString &path) const
{
    const QString dir = imagesDir(path);
    QDir().mkpath(dir);
    int n = int(imageFiles(dir).size());
    QString file;
    do {
        file = QDir(dir).filePath(QStringLiteral("IMG_%1.jpg").arg(n++, 4, 10, QLatin1Char('0')));
    } while (QFileInfo::exists(file) || QFileInfo::exists(QString(file).replace(QStringLiteral(".jpg"), QStringLiteral(".png"))));
    return file;
}

QVariantMap ProjectModel::info(const QString &path) const
{
    const Entry e = readEntry(path);
    QVariantMap m;
    m.insert(QStringLiteral("name"), e.name);
    m.insert(QStringLiteral("path"), e.path);
    m.insert(QStringLiteral("source"), e.source);
    m.insert(QStringLiteral("imageCount"), e.imageCount);
    m.insert(QStringLiteral("hasResult"), e.hasResult);
    m.insert(QStringLiteral("hasArcorePoses"), QFileInfo::exists(QDir(path).filePath(QStringLiteral("arcore/cameras.txt"))));
    m.insert(QStringLiteral("created"), QLocale().toString(e.created, QLocale::ShortFormat));
    m.insert(QStringLiteral("thumbnail"), e.firstImage.isEmpty() ? QUrl() : QUrl::fromLocalFile(e.firstImage));
    m.insert(QStringLiteral("resultUrl"), resultUrl(path));
    QFile log(QDir(path).filePath(QStringLiteral("log.txt")));
    if (log.open(QIODevice::ReadOnly))
        m.insert(QStringLiteral("log"), QString::fromUtf8(log.readAll().right(20000)));
    return m;
}

QUrl ProjectModel::resultUrl(const QString &path) const
{
    const QString ply = QDir(path).filePath(QStringLiteral("output/splat.ply"));
    return QFileInfo::exists(ply) ? QUrl::fromLocalFile(ply) : QUrl();
}

bool ProjectModel::backupResult(const QString &path)
{
    const QString full = QDir(path).filePath(QStringLiteral("output/splat_full.ply"));
    if (QFileInfo::exists(full))
        return true; // keep the original from the first crop
    return QFile::copy(QDir(path).filePath(QStringLiteral("output/splat.ply")), full);
}

bool ProjectModel::hasFullBackup(const QString &path) const
{
    return QFileInfo::exists(QDir(path).filePath(QStringLiteral("output/splat_full.ply")));
}

bool ProjectModel::restoreFullResult(const QString &path)
{
    const QString full = QDir(path).filePath(QStringLiteral("output/splat_full.ply"));
    const QString result = QDir(path).filePath(QStringLiteral("output/splat.ply"));
    if (!QFileInfo::exists(full))
        return false;
    QFile::remove(result);
    const bool ok = QFile::copy(full, result);
    refresh();
    return ok;
}
