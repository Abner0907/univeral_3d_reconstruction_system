#include "ImageOrientation.h"

#include "Pipeline.h"

#include <QDir>
#include <QImage>
#include <QImageIOHandler>
#include <QImageReader>

namespace u3d {

int normalizeImageOrientation(const QString &dir, const std::atomic_bool *cancel)
{
    int rewritten = 0;
    const QFileInfoList files = QDir(dir).entryInfoList(QDir::Files, QDir::Name);
    for (const QFileInfo &info : files) {
        if (cancel && cancel->load())
            break;
        if (!pipeline::isImageFile(info.fileName()))
            continue;

        QImage upright;
        {
            QImageReader reader(info.absoluteFilePath());
            if (!reader.canRead() || reader.transformation() == QImageIOHandler::TransformationNone)
                continue;
            reader.setAutoTransform(true);
            upright = reader.read();
        } // close the file before overwriting it (required on Windows)

        if (upright.isNull())
            continue;
        // QImage does not write EXIF, so the orientation tag is dropped with the rotation applied.
        if (upright.save(info.absoluteFilePath(), nullptr, 95))
            ++rewritten;
    }
    return rewritten;
}

} // namespace u3d
