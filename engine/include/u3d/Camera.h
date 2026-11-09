#pragma once

#include <array>
#include <functional>
#include <string>

namespace u3d {

// Pinhole camera. Pose is world-to-camera in the OpenCV convention:
// x right, y down, z forward;  p_cam = R * p_world + t.
struct Camera {
    int width = 0;
    int height = 0;
    double fx = 0, fy = 0, cx = 0, cy = 0;
    std::array<double, 9> R{1, 0, 0, 0, 1, 0, 0, 0, 1}; // row-major
    std::array<double, 3> t{0, 0, 0};

    // Camera centre in world coordinates (-R^T t).
    std::array<double, 3> center() const;
    // The same camera for an image resized by `scale`.
    Camera scaled(double scale) const;
    // Projects a world point; returns false if it is behind the camera.
    bool project(const std::array<double, 3> &world, double &u, double &v) const;
};

// Progress reporting shared by the pipeline stages.
struct Progress {
    std::string stage;   // "images", "poses", "training", "export"
    double fraction = 0; // overall 0..1
    std::string message;
};
using ProgressCallback = std::function<void(const Progress &)>;
using LogCallback = std::function<void(const std::string &)>;

} // namespace u3d
