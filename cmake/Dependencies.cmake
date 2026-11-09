# Third-party dependencies of the reconstruction engine.
#
#   OpenCV (core imgproc imgcodecs features2d calib3d flann) - feature detection/matching,
#          two-view geometry, PnP. On Android point OpenCV_DIR at
#          <OpenCV-android-sdk>/sdk/native/jni.
#   Eigen 3.4 - bundle adjustment. Uses an installed Eigen if found, otherwise downloads it.

find_package(Threads REQUIRED)

if(U3D_WITH_SFM)
    if(ANDROID)
        # The OpenCV Android SDK bundles all modules into one shared library (libopencv_java4.so).
        find_package(OpenCV 4 REQUIRED COMPONENTS opencv_java)
    else()
        find_package(OpenCV 4 REQUIRED COMPONENTS core imgproc imgcodecs features2d calib3d flann)
    endif()
    message(STATUS "OpenCV ${OpenCV_VERSION}: ${OpenCV_DIR}")

    find_package(Eigen3 3.3 QUIET NO_MODULE)
    if(NOT TARGET Eigen3::Eigen)
        include(FetchContent)
        FetchContent_Declare(eigen
            URL https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz
            URL_HASH SHA256=8586084f71f9bde545ee7fa6d00288b264a2b7ac3607b974e54d13e7162c1c72
            SOURCE_SUBDIR do-not-configure)
        FetchContent_MakeAvailable(eigen)
        add_library(Eigen3::Eigen INTERFACE IMPORTED GLOBAL)
        set_target_properties(Eigen3::Eigen PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${eigen_SOURCE_DIR}")
        message(STATUS "Eigen: downloaded to ${eigen_SOURCE_DIR}")
    else()
        message(STATUS "Eigen ${Eigen3_VERSION}: ${Eigen3_DIR}")
    endif()
endif()
