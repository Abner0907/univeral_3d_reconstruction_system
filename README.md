# Universal 3D Reconstruction System

Turn photos into 3D. Photos come from an Android phone (live CameraX capture or the gallery)
or from the desktop. They are reconstructed with
[Nerfstudio](https://docs.nerf.studio/), using **Gaussian Splatting** (`splatfacto`,
rasterised by [gsplat](https://github.com/nerfstudio-project/gsplat)) or **NeRF** (`nerfacto`).
You can then view the result in a Qt desktop app.

```
 ┌──────────────────────────┐   HTTP (LAN)    ┌──────────────────────────────────────────────┐
 │ Android: U3D Capture      │ ─────────────▶ │ U3D Studio (Qt GUI)  or  u3d_server (headless)  │
 │ Kotlin + CameraX          │  upload images │  C++ / Qt: JobManager + REST API                │
 │ • capture / auto-capture  │ ◀───────────── │   ├─ ns-process-data  (COLMAP camera poses)     │
 │ • pick from gallery       │   progress     │   ├─ ns-train splatfacto | nerfacto  (gsplat)   │
 └──────────────────────────┘                 │   └─ ns-export → result .ply → 3D viewer        │
                                               └──────────────────────────────────────────────┘
```

All of our code is **C++ (Qt)**, plus **Kotlin** for the Android app. Nerfstudio is a Python
package, so the C++ side runs its command-line tools (`ns-process-data`, `ns-train`,
`ns-export`) as subprocesses. Our code contains no Python.

| Component | Tech | Path |
|---|---|---|
| Core library: jobs, pipeline, HTTP server, PLY loader | C++17, Qt Core/Gui/Network | [src/core](src/core) |
| **U3D Studio**: desktop app with job queue, logs and OpenGL 3D viewer | Qt Widgets + OpenGL | [src/gui](src/gui) |
| **u3d_server**: headless server for GPU machines | Qt Core | [src/server](src/server) |
| **u3d_fake_ns**: Nerfstudio simulator for dry runs and CI | C++17 | [tools/fake_ns.cpp](tools/fake_ns.cpp) |
| **U3D Capture**: Android app | Kotlin, CameraX, OkHttp | [android](android) |
| CI and releases | GitHub Actions | [.github/workflows/ci.yml](.github/workflows/ci.yml) |

## Quick start (no GPU needed)

The **dry-run** mode replaces Nerfstudio with `u3d_fake_ns`, which produces a coloured sphere.
Use it to try the whole flow, including the phone upload, on any machine:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

U3D_DRY_RUN=1 ./build/bin/U3DStudio          # or: ./build/bin/u3d_server --dry-run
```

## Real reconstructions

1. **Install Nerfstudio** on a machine with an NVIDIA GPU (see the
   [Nerfstudio installation guide](https://docs.nerf.studio/quickstart/installation.html)). It
   installs `gsplat` (used by splatfacto). COLMAP and FFmpeg must also be installed:
   ```bash
   conda create -n nerfstudio python=3.10 -y && conda activate nerfstudio
   pip install torch torchvision --index-url https://download.pytorch.org/whl/cu118
   pip install nerfstudio
   conda install -c conda-forge colmap ffmpeg
   ```
2. **Build and start U3D Studio** (Qt 6.5 or newer recommended; Qt 5.15 also works).
3. Open **File → Preferences** and set **Nerfstudio bin folder** to the environment's `bin`
   directory, for example `~/miniconda3/envs/nerfstudio/bin`. You can skip this if `ns-train`
   is already on `PATH`. Make sure **dry run** is turned off.
4. Add photos (**Images…**, **Folder…**, **Video…**, or drag and drop), pick a method, then
   press **Start reconstruction**. Follow progress in the **Log** tab. While training runs,
   **Live viewer** opens the Nerfstudio web viewer.
5. When the job finishes, **View 3D** shows the result. **Save PLY…** exports it for other
   tools, such as SuperSplat or other Gaussian-splat viewers.

**Pipeline per job.** Each stage is one subprocess, and its output streams into the job log:

| Stage | Command | Progress |
|---|---|---|
| prepare | Applies EXIF rotation to the image pixels, because COLMAP ignores EXIF orientation | 0–2 % |
| process_data | `ns-process-data images\|video --matching-method …` | 2–15 % |
| train | `ns-train splatfacto\|nerfacto --max-num-iterations N --vis viewer …` | 15–95 % (parsed from the `Step (% Done)` table) |
| export | `ns-export gaussian-splat` (splats) or `ns-export pointcloud` (NeRF) | 95–100 % |

Jobs run one at a time, because a single GPU is assumed. Each job is stored in
`<data dir>/jobs/<id>/`, so jobs survive restarts.

## Capturing with the Android app

1. Install the APK from the CI artifacts or a release, or build it with
   `cd android && ./gradlew assembleDebug`.
2. Put the phone on the same Wi-Fi as the computer. U3D Studio's status bar shows the address
   to use, for example `Phone uploads: http://192.168.1.20:8000`. Enter it in the app and tap
   **Check**.
3. Walk slowly around the object. Take 30–200 overlapping photos, or turn on **Auto** to
   capture one every second. You can also add pictures with **Gallery**. HEIC and WebP images
   are converted to JPEG.
4. Tap **Reconstruct**. The app uploads the images one at a time, starts the job and shows its
   progress. The job also appears live in U3D Studio.

Capture tips: keep the lighting consistent, avoid motion blur, overlap neighbouring shots by
about 60–80 %, and include texture (plain white surfaces reconstruct poorly).

## Headless server

For a remote GPU box without a desktop:

```bash
./build/bin/u3d_server --port 8000 --ns-bin-dir ~/miniconda3/envs/nerfstudio/bin --data-dir ~/u3d-data
```

Options can also be set with environment variables: `U3D_DATA_DIR`, `U3D_NS_BIN_DIR`,
`U3D_DRY_RUN`, `U3D_NS_VIS`, `U3D_MATCHING_METHOD`, `U3D_DEFAULT_METHOD`,
`U3D_DEFAULT_ITERATIONS`, `U3D_VIDEO_FRAMES` and `U3D_PORT`.

There is also a Docker image definition, based on the official Nerfstudio image (CUDA, COLMAP
and gsplat included). CI does not build it.

```bash
docker build -f packaging/Dockerfile -t u3d-server .
docker run --gpus all -p 8000:8000 -p 7007:7007 -v $PWD/u3d-data:/data u3d-server
```

## REST API

U3D Studio (when **Phone → Accept phone uploads** is on) and `u3d_server` serve:

| Method | Path | |
|---|---|---|
| GET | `/api/info` | version, methods, defaults, `dry_run` |
| GET | `/api/jobs` | `{"jobs": [...]}`, newest first |
| POST | `/api/jobs` | `{"name","method","iterations"}` → job with status `uploading` |
| PUT | `/api/jobs/{id}/files/{name}` | raw bytes of one image or video (needs `Content-Length`) |
| POST | `/api/jobs/{id}/start` | validates inputs and queues the job |
| GET | `/api/jobs/{id}` | `status`, `stage`, `progress` (0–1), `message`, `has_result` |
| GET | `/api/jobs/{id}/log?tail=65536` | text log |
| POST | `/api/jobs/{id}/cancel` | |
| DELETE | `/api/jobs/{id}` | deletes the job and its files |
| GET | `/api/jobs/{id}/result` | the result `.ply` |

Example with curl:

```bash
ID=$(curl -s -X POST localhost:8000/api/jobs -d '{"method":"splatfacto"}' | jq -r .id)
for f in photos/*.jpg; do curl -s -X PUT --data-binary @"$f" "localhost:8000/api/jobs/$ID/files/$(basename "$f")"; done
curl -s -X POST localhost:8000/api/jobs/$ID/start
```

The API has **no authentication**. Run it only on a trusted local network.

## CI and releases

[.github/workflows/ci.yml](.github/workflows/ci.yml) runs on every push and pull request:

- **Qt Linux / Windows / macOS**: installs Qt 6.8, builds everything, runs the four test
  suites (`ctest`), and packages an AppImage plus server tarball, a Windows zip (via
  `windeployqt`) and a macOS dmg (via `macdeployqt`).
- **Android**: runs `./gradlew assembleDebug testDebugUnitTest` and uploads the APK.
- **Release**: pushing a tag such as `v0.1.0` attaches all of these artifacts to a GitHub
  release.

## Project layout

```
CMakeLists.txt            single CMake project (GUI, server, simulator, tests)
src/core/                 Settings, Job, Pipeline, JobManager, ImageOrientation,
                          HttpServer, ApiService, PlyLoader
src/gui/                  MainWindow, PointCloudViewer (OpenGL), PreferencesDialog
src/server/main.cpp       headless server
tools/fake_ns.cpp         Nerfstudio simulator
tests/                    QtTest: pipeline, PLY loader, job manager, HTTP API (end to end)
android/                  Gradle project for the Kotlin + CameraX capture app
packaging/                .desktop file, icon, Dockerfile
```

## Known limitations

- The desktop viewer draws each Gaussian as a coloured point, which is good for inspection.
  For full-quality splat rendering, use **Live viewer** during training, or open the exported
  `.ply` in a dedicated splat viewer.
- Uploads are held in memory per file, with a 1 GiB maximum per request.
