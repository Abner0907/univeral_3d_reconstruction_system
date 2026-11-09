// u3d_reconstruct - run the on-device reconstruction pipeline on a project folder from the
// command line (development, benchmarking, testing the same code the Android app runs).
//
//   u3d_reconstruct <project-dir> [--quality fast|balanced|high] [--threads N] [--iterations N] [--force-sfm]
//
// <project-dir>/images/ holds the photos; the result goes to <project-dir>/output/splat.ply.

#include "u3d/Reconstructor.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char **argv)
{
    if (argc < 2 || std::strcmp(argv[1], "--help") == 0) {
        std::fprintf(stderr, "usage: %s <project-dir> [--quality fast|balanced|high] [--threads N] [--iterations N] [--force-sfm]\n", argv[0]);
        return 2;
    }
    u3d::ReconstructOptions options;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--quality" && i + 1 < argc) {
            const std::string q = argv[++i];
            options.quality = q == "fast" ? u3d::Quality::Fast : q == "high" ? u3d::Quality::High : u3d::Quality::Balanced;
        } else if (a == "--threads" && i + 1 < argc) {
            options.threads = std::atoi(argv[++i]);
        } else if (a == "--iterations" && i + 1 < argc) {
            options.iterations = std::atoi(argv[++i]);
        } else if (a == "--force-sfm") {
            options.forceSfm = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 2;
        }
    }

    std::string lastStage;
    int lastPercent = -1;
    u3d::ReconstructResult result;
    std::string error;
    const bool ok = u3d::reconstructProject(
        argv[1], options, result,
        [&](const u3d::Progress &p) {
            const int percent = int(p.fraction * 100);
            if (percent != lastPercent || p.stage != lastStage) {
                std::fprintf(stderr, "\r[%3d%%] %-70s", percent, p.message.c_str());
                lastPercent = percent;
                lastStage = p.stage;
            }
        },
        [](const std::string &line) { std::fprintf(stderr, "\n%s", line.c_str()); },
        nullptr, &error);
    std::fprintf(stderr, "\n");
    if (!ok) {
        std::fprintf(stderr, "Reconstruction failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("%s\n", result.splatPath.c_str());
    return 0;
}
