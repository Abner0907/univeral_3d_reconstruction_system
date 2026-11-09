# U3D - Qt Quick app.
#   Android: capture (ARCore or Qt Multimedia camera) + on-device reconstruction + splat viewer.
#   Linux / Windows: splat viewer (open .ply files).

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Multimedia Concurrent OpenGL)
qt_standard_project_setup(REQUIRES 6.5)
if(QT_KNOWN_POLICY_QTP0004)
    qt_policy(SET QTP0004 NEW)
endif()

qt_add_executable(u3d_app
    src/main.cpp
)
set_target_properties(u3d_app PROPERTIES
    OUTPUT_NAME U3D
    WIN32_EXECUTABLE ON
    QT_ANDROID_PACKAGE_NAME "com.universal3d.reconstruct"
    QT_ANDROID_APP_NAME "U3D"
    QT_ANDROID_VERSION_NAME "${PROJECT_VERSION}"
    QT_ANDROID_VERSION_CODE 2
    QT_ANDROID_MIN_SDK_VERSION 28
    QT_ANDROID_TARGET_SDK_VERSION 34)

qt_add_qml_module(u3d_app
    URI U3D
    VERSION 1.0
    # Keep the module's build folder apart from the executable, which is also named U3D.
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/qml_modules/U3D"
    QML_FILES
        qml/Main.qml
        qml/HomePage.qml
        qml/ProjectPage.qml
        qml/CapturePage.qml
        qml/ReconstructPage.qml
        qml/ViewerPage.qml
    SOURCES
        src/AppInfo.h src/AppInfo.cpp
        src/ProjectModel.h src/ProjectModel.cpp
        src/ReconstructionController.h src/ReconstructionController.cpp
        src/SplatView.h src/SplatView.cpp
        src/ArCapture.h src/ArCapture.cpp
)

target_include_directories(u3d_app PRIVATE src)
target_compile_definitions(u3d_app PRIVATE U3D_APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(u3d_app PRIVATE
    Qt6::Quick Qt6::QuickControls2 Qt6::Multimedia Qt6::Concurrent Qt6::OpenGL
    u3d_splat)

if(U3D_WITH_SFM)
    target_link_libraries(u3d_app PRIVATE u3d_sfm)
    target_compile_definitions(u3d_app PRIVATE U3D_HAS_RECONSTRUCTION=1)
endif()

if(ANDROID)
    # ARCore's C API header (libraries/include/arcore_c_api.h of google-ar/arcore-android-sdk).
    # The library itself comes from the com.google.ar:core AAR and is loaded at runtime with
    # dlopen, so the app also runs on phones without ARCore.
    set(U3D_ARCORE_INCLUDE_DIR "" CACHE PATH "Folder containing arcore_c_api.h")
    if(U3D_ARCORE_INCLUDE_DIR AND EXISTS "${U3D_ARCORE_INCLUDE_DIR}/arcore_c_api.h")
        target_sources(u3d_app PRIVATE src/ArCoreSession.h src/ArCoreSession.cpp)
        target_include_directories(u3d_app PRIVATE "${U3D_ARCORE_INCLUDE_DIR}")
        target_compile_definitions(u3d_app PRIVATE U3D_HAS_ARCORE=1)
        target_link_libraries(u3d_app PRIVATE dl)
    else()
        message(WARNING "arcore_c_api.h not found (U3D_ARCORE_INCLUDE_DIR): building without ARCore")
    endif()

    # Android package sources: our manifest + Qt's own build.gradle template with the ARCore
    # dependency added (so it always matches the Qt version in use).
    set(U3D_ANDROID_PACKAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/android-package")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/android/" DESTINATION "${U3D_ANDROID_PACKAGE_DIR}")
    set(_gradle_template "")
    foreach(candidate
            "${QT6_INSTALL_PREFIX}/src/android/templates/build.gradle"
            "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_DATA}/src/android/templates/build.gradle")
        if(EXISTS "${candidate}")
            set(_gradle_template "${candidate}")
            break()
        endif()
    endforeach()
    if(_gradle_template)
        file(READ "${_gradle_template}" _gradle)
        string(REGEX REPLACE "dependencies[ \t]*{" "dependencies {\n    implementation 'com.google.ar:core:1.47.0'" _gradle "${_gradle}")
        file(WRITE "${U3D_ANDROID_PACKAGE_DIR}/build.gradle" "${_gradle}")
        message(STATUS "Android: build.gradle from ${_gradle_template} + ARCore")
    else()
        message(WARNING "Qt's Android build.gradle template not found: the APK will not include ARCore")
    endif()
    set_property(TARGET u3d_app PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR "${U3D_ANDROID_PACKAGE_DIR}")

    # OpenCV's shared library from the OpenCV Android SDK must ship inside the APK.
    if(U3D_WITH_SFM)
        file(GLOB _opencv_so "${OpenCV_DIR}/../libs/${ANDROID_ABI}/libopencv_java4.so")
        if(_opencv_so)
            set_property(TARGET u3d_app APPEND PROPERTY QT_ANDROID_EXTRA_LIBS ${_opencv_so})
        else()
            message(WARNING "libopencv_java4.so not found next to OpenCV_DIR=${OpenCV_DIR}")
        endif()
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS u3d_app
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(UNIX AND NOT APPLE AND NOT ANDROID)
    install(FILES packaging/u3d.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES packaging/u3d.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()

# Development tool: render a .ply with SplatView offscreen (no window) to a PNG.
if(NOT ANDROID)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(u3d_render_check tools/render_check.cpp src/SplatView.h src/SplatView.cpp)
    target_include_directories(u3d_render_check PRIVATE src)
    target_link_libraries(u3d_render_check PRIVATE Qt6::Quick Qt6::OpenGL Qt6::Concurrent Qt6::Test u3d_splat)
    set_target_properties(u3d_render_check PROPERTIES WIN32_EXECUTABLE OFF)
endif()
