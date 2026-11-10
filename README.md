# Universal 3D Reconstruction System

U3D is a 3D scanner that runs entirely on an Android phone. You take photos with the phone's
camera (with ARCore tracking where available) or pick them from the gallery. The phone then
reconstructs a **3D Gaussian Splatting** model and shows it in the app. No PC, server or cloud
is involved.

The same Qt app builds for **Linux and Windows as a viewer** for the exported `.ply` files.

```
 Phone (Qt Quick app, C++)
 ┌────────────────────────────────────────────────────────────────────────────┐
 │ Capture                Camera poses               Training          View    │
 │ ARCore keyframes ───▶ ARCore poses + points ─┐                              │
 │   or camera / gallery ─▶ SIFT + incremental ──┴─▶ 3D Gaussian ──▶ splat.ply ─▶ │
 │                          SfM + bundle adj.        Splatting      splat viewer │
 └────────────────────────────────────────────────────────────────────────────┘
```

## Why the reconstruction is not Nerfstudio

Nerfstudio and gsplat are Python/PyTorch programs that need an NVIDIA GPU with CUDA. Phones
have neither, so this project reimplements the same algorithms in portable C++:

| Step | Algorithm | Code |
|---|---|---|
| Camera poses (photos only) | RootSIFT features, ratio test, fundamental-matrix RANSAC, conflict-free track building, incremental SfM (essential-matrix initial pair, EPnP + RANSAC registration, DLT triangulation), Levenberg-Marquardt bundle adjustment with Schur complement, Huber loss and shared focal length | [engine/src/sfm](engine/src/sfm) |
| Camera poses (ARCore) | Tracked 6-DoF pose and intrinsics per keyframe, plus ARCore feature points | [app/src/ArCoreSession.cpp](app/src/ArCoreSession.cpp) |
| Training | 3D Gaussian Splatting as in splatfacto/gsplat: EWA projection, tile-based depth-sorted alpha compositing, analytic backward pass, L1 + 0.2·D-SSIM loss, Adam, clone/split/prune densification, opacity reset | [engine/src/splat](engine/src/splat) |
| Output | Standard 3DGS `.ply` layout (opens in common splat viewers) | [SplatPly.cpp](engine/src/splat/SplatPly.cpp) |
| Viewer | Instanced, depth-sorted, alpha-blended splat ellipses (OpenGL ES 3.0 / GL 3.3) | [app/src/SplatView.cpp](app/src/SplatView.cpp) |

Training currently runs on the phone's CPU cores, multithreaded. A GPU (Vulkan compute) trainer
is the planned next step; the multithreaded CPU trainer, verified against finite differences,
is its reference implementation.

## Using the app (Android)

1. **New scan.** The normal camera starts immediately. Walk slowly around the object; with
   **Auto** on, a photo is taken every second (or tap the shutter). You can also use
   **Import photos** to pick 30–200 overlapping photos from the gallery.
   - **ARCore** (optional): on phones where Google Play Services for AR is available, an
     **ARCore** switch appears in the capture toolbar. With it on, each photo also records the
     tracked camera pose and feature points (Auto then saves a keyframe every ~7 cm / 7°), and
     reconstruction can skip structure-from-motion. If ARCore fails or delivers no camera
     images, the app switches back to the normal camera by itself.
2. **Reconstruct.** Choose a quality preset. Everything runs on the phone and the screen stays
   on meanwhile:

   | Preset | Iterations | Training resolution | Max Gaussians |
   |---|---|---|---|
   | Fast | 2000 | 320 px | 80 k |
   | Balanced | 4000 | 480 px | 150 k |
   | High | 7000 | 640 px | 300 k |

3. **View** the model: one finger orbits, two fingers pan, pinch zooms.
4. **Crop** (viewer toolbar) to keep only the object: X / Y / Z range sliders define a box
   (drawn as a wireframe; everything outside is hidden live), **Remove faint** drops nearly
   transparent splats, and **Auto** fits the box around the main object, ignoring background
   and floaters. **Apply** saves the cropped model; the original is kept and **Restore
   original** brings it back.
5. **Export .ply** to share it.

Capture tips: keep the lighting even, avoid motion blur, overlap neighbouring photos by about
70 %, and prefer textured surfaces (plain white walls and reflective or transparent objects
reconstruct poorly).

## Desktop viewer (Linux / Windows)

`U3D file.ply`, or **Open .ply** in the app. Left-drag orbits, right-drag pans, the wheel zooms.
For development, `U3D_DEV_RECONSTRUCT=1 U3D` also enables scanning and reconstruction on a
desktop build configured with `-DU3D_WITH_SFM=ON`.

## Building

Requirements: CMake 3.21+, a C++17 compiler, Qt 6.5+ (Quick, QuickControls2, Multimedia),
and for reconstruction OpenCV 4 and Eigen 3.4 (Eigen is downloaded automatically if it is
missing).

```bash
# Engine + tests + command-line tool (Linux: apt install libopencv-dev libeigen3-dev)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DU3D_BUILD_APP=OFF
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/engine/u3d_reconstruct my_project --quality fast    # my_project/images/*.jpg

# Desktop viewer (no OpenCV needed)
cmake -S . -B build-viewer -DCMAKE_PREFIX_PATH=~/Qt/6.8.3/gcc_64 -DU3D_WITH_SFM=OFF
cmake --build build-viewer -j

# Android APK (see .github/workflows/ci.yml for the exact steps)
~/Qt/6.8.3/android_arm64_v8a/bin/qt-cmake -S . -B build-android -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DQT_ANDROID_ABIS=arm64-v8a \
  -DANDROID_SDK_ROOT=$ANDROID_SDK_ROOT -DANDROID_NDK_ROOT=$ANDROID_SDK_ROOT/ndk/26.1.10909125 \
  -DOpenCV_DIR=$PWD/deps/OpenCV-android-sdk/sdk/native/jni \
  -DU3D_ARCORE_INCLUDE_DIR=$PWD/deps/arcore
cmake --build build-android --target apk
```

For Android you need the [OpenCV Android SDK](https://github.com/opencv/opencv/releases) and
ARCore's `arcore_c_api.h`, from `libraries/include` in
[arcore-android-sdk](https://github.com/google-ar/arcore-android-sdk). The ARCore library comes
from the `com.google.ar:core` Gradle dependency, which is added to Qt's own `build.gradle`
template at configure time. The app loads it at runtime, so it also works on phones without
ARCore.

## Tests

`ctest` runs [tests/](tests):

- **splat_gradients**: analytic gradients of all parameters (means, scales, rotations,
  opacities, colours) match central finite differences (relative error below 0.1 %).
- **splat_projection**: a rendered Gaussian matches the closed-form projected ellipse.
- **ssim_loss**: the D-SSIM gradient check.
- **training_recovers**: training on a synthetic scene raises PSNR from about 20 dB to over
  26 dB.
- **ply_round_trip**: writing and reading splat and point-cloud `.ply` files.
- **bundle_adjustment**: on noisy synthetic data, the reprojection error drops from 16 px to
  0.6 px and the focal length is recovered to within 0.1 %.
- **incremental_sfm**: all 20 synthetic cameras are recovered, with camera centres within
  0.04 % of the orbit radius.
- **pipeline_with_arcore_poses** and **pipeline_with_structure_from_motion**: 30 rendered JPEG
  photos of a textured box go through the full phone pipeline (29/30 photos registered by SfM).

## CI

[.github/workflows/ci.yml](.github/workflows/ci.yml): engine tests (Linux), the viewer for
Linux (AppImage) and Windows (zip), and the Android APK (arm64, signed with a throwaway debug
key). Pushing a tag `v*` publishes all artifacts as a GitHub release.

## Project layout

```
engine/include/u3d/      public headers: Camera, Project, Reconstructor, splat/*, sfm/*
engine/src/splat/        Gaussian model, rasterizer (forward+backward), loss, Adam, trainer, PLY
engine/src/sfm/          features + matching, incremental SfM, bundle adjustment
engine/src/pipeline/     project folder format, end-to-end reconstruction
engine/tools/            u3d_reconstruct command-line tool
app/src/                 Qt app: projects, reconstruction controller, splat viewer, ARCore capture
app/qml/                 user interface
app/android/             Android manifest and icon
tests/                   engine tests
```

A project folder contains `images/`, an optional `arcore/` folder (`cameras.txt`, `points.ply`)
and `output/` (`splat.ply`, `cameras.txt`, `sparse.ply`). The format is documented in
[Project.h](engine/include/u3d/Project.h).
