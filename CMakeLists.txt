cmake_minimum_required(VERSION 3.21)

project(Universal3DReconstruction
    VERSION 0.2.0
    DESCRIPTION "On-device 3D reconstruction (structure-from-motion + Gaussian Splatting) with Qt"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# u3d_splat (Gaussian Splatting trainer/renderer core) is always built.
# U3D_WITH_SFM adds structure-from-motion and the full reconstruction pipeline (OpenCV + Eigen).
# The Android app needs it; the Linux/Windows viewer does not.
if(ANDROID)
    set(_u3d_default_tests OFF)
else()
    set(_u3d_default_tests ON)
endif()
option(U3D_WITH_SFM "Build structure-from-motion and the reconstruction pipeline (OpenCV + Eigen)" ON)
option(U3D_BUILD_APP "Build the Qt Quick app (Android: capture + reconstruct + view; desktop: viewer)" ON)
option(U3D_BUILD_TESTS "Build engine tests" ${_u3d_default_tests})

include(cmake/Dependencies.cmake)

add_subdirectory(engine)

if(U3D_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(U3D_BUILD_APP)
    add_subdirectory(app)
endif()
