#pragma once

#include "u3d/Camera.h"

#include <string>
#include <vector>

namespace u3d {

// A capture project on disk:
//
//   <project>/images/*.jpg         photos (camera captures or imported from the gallery)
//   <project>/arcore/cameras.txt   optional per-image intrinsics + pose from ARCore tracking
//   <project>/arcore/points.ply    optional sparse ARCore feature points
//   <project>/output/splat.ply     result (3D Gaussian Splatting)
//   <project>/output/cameras.txt   final camera poses
//   <project>/output/sparse.ply    sparse point cloud used for initialisation
//
// cameras.txt: one line per image, '#' comments:
//   name width height fx fy cx cy r00 r01 r02 r10 r11 r12 r20 r21 r22 tx ty tz
// with the world-to-camera pose in the OpenCV convention (see Camera.h).
struct ProjectData {
    std::string dir;
    std::vector<std::string> images;  // file names in images/, sorted (= capture order)
    bool hasPoses = false;            // every image has an ARCore pose
    std::vector<Camera> poses;        // aligned with `images` when hasPoses
    std::vector<float> pointsXyz;     // optional ARCore points
    std::vector<float> pointsRgb;
};

bool loadProject(const std::string &dir, ProjectData &project, std::string *error = nullptr);

bool writeCamerasTxt(const std::string &path, const std::vector<std::string> &names,
                     const std::vector<Camera> &cameras, std::string *error = nullptr);
// Returns cameras keyed by image name order of `names`; missing entries leave found[i] false.
bool readCamerasTxt(const std::string &path, const std::vector<std::string> &names,
                    std::vector<Camera> &cameras, std::vector<bool> &found, std::string *error = nullptr);

std::string joinPath(const std::string &a, const std::string &b);

} // namespace u3d
