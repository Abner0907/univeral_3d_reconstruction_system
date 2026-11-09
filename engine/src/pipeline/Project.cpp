#include "u3d/Project.h"

#include "u3d/splat/SplatPly.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace u3d {

namespace {
bool fail(std::string *error, const std::string &message)
{
    if (error)
        *error = message;
    return false;
}

bool isImageName(const std::string &name)
{
    std::string ext = fs::path(name).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png";
}
} // namespace

std::string joinPath(const std::string &a, const std::string &b)
{
    return (fs::path(a) / b).string();
}

bool writeCamerasTxt(const std::string &path, const std::vector<std::string> &names,
                     const std::vector<Camera> &cameras, std::string *error)
{
    std::ofstream out(path);
    if (!out)
        return fail(error, "Cannot write " + path);
    out.imbue(std::locale::classic());
    out << "# name width height fx fy cx cy r00 r01 r02 r10 r11 r12 r20 r21 r22 tx ty tz\n";
    out << "# world-to-camera, OpenCV convention (x right, y down, z forward)\n";
    out << std::setprecision(10);
    for (std::size_t i = 0; i < names.size() && i < cameras.size(); ++i) {
        const Camera &c = cameras[i];
        out << names[i] << ' ' << c.width << ' ' << c.height << ' ' << c.fx << ' ' << c.fy << ' ' << c.cx << ' ' << c.cy;
        for (double r : c.R)
            out << ' ' << r;
        for (double t : c.t)
            out << ' ' << t;
        out << '\n';
    }
    return bool(out) || fail(error, "Write error: " + path);
}

bool readCamerasTxt(const std::string &path, const std::vector<std::string> &names,
                    std::vector<Camera> &cameras, std::vector<bool> &found, std::string *error)
{
    std::ifstream in(path);
    if (!in)
        return fail(error, "Cannot open " + path);
    std::map<std::string, Camera> byName;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ls(line);
        ls.imbue(std::locale::classic());
        std::string name;
        Camera c;
        ls >> name >> c.width >> c.height >> c.fx >> c.fy >> c.cx >> c.cy;
        for (double &r : c.R)
            ls >> r;
        for (double &t : c.t)
            ls >> t;
        if (!ls.fail())
            byName[name] = c;
    }
    cameras.assign(names.size(), Camera());
    found.assign(names.size(), false);
    for (std::size_t i = 0; i < names.size(); ++i) {
        auto it = byName.find(names[i]);
        if (it != byName.end()) {
            cameras[i] = it->second;
            found[i] = true;
        }
    }
    return true;
}

bool loadProject(const std::string &dir, ProjectData &project, std::string *error)
{
    project = ProjectData();
    project.dir = dir;
    const fs::path imagesDir = fs::path(dir) / "images";
    std::error_code ec;
    if (!fs::is_directory(imagesDir, ec))
        return fail(error, "Project has no images folder: " + imagesDir.string());
    for (const auto &entry : fs::directory_iterator(imagesDir, ec))
        if (entry.is_regular_file() && isImageName(entry.path().filename().string()))
            project.images.push_back(entry.path().filename().string());
    std::sort(project.images.begin(), project.images.end());
    if (project.images.empty())
        return fail(error, "The project contains no photos.");

    const fs::path camerasTxt = fs::path(dir) / "arcore" / "cameras.txt";
    if (fs::exists(camerasTxt, ec)) {
        std::vector<bool> found;
        if (readCamerasTxt(camerasTxt.string(), project.images, project.poses, found, error))
            project.hasPoses = std::all_of(found.begin(), found.end(), [](bool b) { return b; });
    }
    const fs::path pointsPly = fs::path(dir) / "arcore" / "points.ply";
    if (fs::exists(pointsPly, ec))
        readPointsPly(pointsPly.string(), project.pointsXyz, project.pointsRgb);
    return true;
}

} // namespace u3d
