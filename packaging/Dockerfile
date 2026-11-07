# Headless reconstruction server: the official Nerfstudio image (CUDA, COLMAP, gsplat) plus the
# C++ u3d_server built against the distribution's Qt 6.
#
#   docker build -f packaging/Dockerfile -t u3d-server .
#   docker run --gpus all -p 8000:8000 -p 7007:7007 -v $PWD/u3d-data:/data u3d-server
FROM ghcr.io/nerfstudio-project/nerfstudio:latest

USER root
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build qt6-base-dev libgl1-mesa-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/u3d/src
COPY CMakeLists.txt ./
COPY src ./src
COPY tools ./tools
COPY tests ./tests
COPY packaging ./packaging
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DU3D_BUILD_GUI=OFF -DU3D_BUILD_TESTS=OFF \
 && cmake --build build \
 && cmake --install build --prefix /usr/local \
 && rm -rf build

ENV U3D_DATA_DIR=/data
VOLUME ["/data"]
EXPOSE 8000 7007
ENTRYPOINT ["u3d_server", "--port", "8000"]
