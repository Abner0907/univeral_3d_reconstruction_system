cmake_minimum_required(VERSION 3.21)

project(Universal3DReconstruction
    VERSION 0.1.0
    DESCRIPTION "Universal 3D Reconstruction System (Qt + Nerfstudio/gsplat)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Put every executable in one folder so the apps can find u3d_fake_ns next to them.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(U3D_BUILD_GUI "Build the U3DStudio desktop application" ON)
option(U3D_BUILD_TESTS "Build unit tests" ON)

# Qt 6 is the target; Qt 5.15 also works.
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
set(U3D_QT Qt${QT_VERSION_MAJOR})
find_package(${U3D_QT} 5.15 REQUIRED COMPONENTS Core Gui Network)
message(STATUS "Using Qt ${${U3D_QT}_VERSION}")

# ----------------------------------------------------------------------------- core library
add_library(u3d_core STATIC
    src/core/Settings.h        src/core/Settings.cpp
    src/core/Job.h             src/core/Job.cpp
    src/core/Pipeline.h        src/core/Pipeline.cpp
    src/core/ImageOrientation.h src/core/ImageOrientation.cpp
    src/core/JobManager.h      src/core/JobManager.cpp
    src/core/HttpServer.h      src/core/HttpServer.cpp
    src/core/ApiService.h      src/core/ApiService.cpp
    src/core/PlyLoader.h       src/core/PlyLoader.cpp
)
target_include_directories(u3d_core PUBLIC src/core)
target_link_libraries(u3d_core PUBLIC ${U3D_QT}::Core ${U3D_QT}::Gui ${U3D_QT}::Network)
target_compile_definitions(u3d_core PUBLIC U3D_VERSION="${PROJECT_VERSION}")

# ----------------------------------------------------------------------------- Nerfstudio simulator
# Stand-in for ns-process-data / ns-train / ns-export used in dry-run mode and tests (no GPU).
add_executable(u3d_fake_ns tools/fake_ns.cpp)

# ----------------------------------------------------------------------------- headless server
add_executable(u3d_server src/server/main.cpp)
target_link_libraries(u3d_server PRIVATE u3d_core)
add_dependencies(u3d_server u3d_fake_ns)

# ----------------------------------------------------------------------------- desktop GUI
if(U3D_BUILD_GUI)
    find_package(${U3D_QT} REQUIRED COMPONENTS Widgets Concurrent)
    set(U3D_GUI_LIBS ${U3D_QT}::Widgets ${U3D_QT}::Concurrent)
    if(QT_VERSION_MAJOR EQUAL 6)
        find_package(Qt6 REQUIRED COMPONENTS OpenGL OpenGLWidgets)
        list(APPEND U3D_GUI_LIBS Qt6::OpenGL Qt6::OpenGLWidgets)
    endif()

    add_executable(u3d_studio WIN32 MACOSX_BUNDLE
        src/gui/main.cpp
        src/gui/MainWindow.h         src/gui/MainWindow.cpp
        src/gui/PointCloudViewer.h   src/gui/PointCloudViewer.cpp
        src/gui/PreferencesDialog.h  src/gui/PreferencesDialog.cpp
    )
    target_link_libraries(u3d_studio PRIVATE u3d_core ${U3D_GUI_LIBS})
    set_target_properties(u3d_studio PROPERTIES
        OUTPUT_NAME U3DStudio
        MACOSX_BUNDLE_BUNDLE_NAME "U3D Studio"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.universal3d.studio"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
    add_dependencies(u3d_studio u3d_fake_ns)
endif()

# ----------------------------------------------------------------------------- install
include(GNUInstallDirs)
install(TARGETS u3d_server u3d_fake_ns RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(U3D_BUILD_GUI)
    install(TARGETS u3d_studio
        BUNDLE DESTINATION .
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(UNIX AND NOT APPLE)
        install(FILES packaging/u3d-studio.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
        install(FILES packaging/u3d-studio.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    endif()
endif()

# ----------------------------------------------------------------------------- tests
if(U3D_BUILD_TESTS)
    enable_testing()
    find_package(${U3D_QT} REQUIRED COMPONENTS Test)

    function(u3d_add_test name)
        add_executable(${name} tests/${name}.cpp)
        target_link_libraries(${name} PRIVATE u3d_core ${U3D_QT}::Test)
        target_compile_definitions(${name} PRIVATE
            U3D_FAKE_NS_PATH="$<TARGET_FILE:u3d_fake_ns>")
        add_dependencies(${name} u3d_fake_ns)
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;U3D_FAKE_STEP_DELAY=0.01"
            TIMEOUT 120)
    endfunction()

    u3d_add_test(tst_pipeline)
    u3d_add_test(tst_plyloader)
    u3d_add_test(tst_jobmanager)
    u3d_add_test(tst_apiservice)
endif()
