#pragma once

#include <QString>

#include <atomic>

namespace u3d {

// Phones store rotation as an EXIF tag, which COLMAP ignores, so a mix of portrait and
// landscape shots (or every shot from a phone held upright) would confuse pose estimation.
// This bakes the EXIF orientation into the pixels of every image in `dir` that needs it.
// Returns the number of rewritten images. `cancel` may be null.
int normalizeImageOrientation(const QString &dir, const std::atomic_bool *cancel = nullptr);

} // namespace u3d
