# ---------------------------------------------------------------- u3d_splat
# Gaussian Splatting core: model, CPU rasterizer (forward + backward), loss, Adam, trainer, PLY.
# Pure C++17, no third-party dependencies.
add_library(u3d_splat STATIC
    src/Camera.cpp
    src/ThreadPool.cpp
    src/splat/GaussianModel.cpp
    src/splat/Rasterizer.cpp
    src/splat/Loss.cpp
    src/splat/Adam.cpp
    src/splat/Trainer.cpp
    src/splat/SplatPly.cpp
)
target_include_directories(u3d_splat PUBLIC include)
target_link_libraries(u3d_splat PUBLIC Threads::Threads)
if(MSVC)
    target_compile_options(u3d_splat PRIVATE /fp:fast)
    target_compile_definitions(u3d_splat PUBLIC _USE_MATH_DEFINES NOMINMAX)
else()
    # The rasterizer is the hot loop of on-device training.
    target_compile_options(u3d_splat PRIVATE $<$<CONFIG:Release,RelWithDebInfo>:-O3>)
endif()

# ---------------------------------------------------------------- u3d_sfm + pipeline
if(U3D_WITH_SFM)
    add_library(u3d_sfm STATIC
        src/sfm/BundleAdjustment.cpp
        src/sfm/Features.cpp
        src/sfm/IncrementalSfM.cpp
        src/pipeline/Project.cpp
        src/pipeline/Reconstructor.cpp
    )
    target_include_directories(u3d_sfm PUBLIC include ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(u3d_sfm PUBLIC u3d_splat ${OpenCV_LIBS} Eigen3::Eigen)
    if(NOT MSVC)
        target_compile_options(u3d_sfm PRIVATE $<$<CONFIG:Release,RelWithDebInfo>:-O3>)
    endif()

    if(NOT ANDROID)
        # Command-line reconstruction of a project folder (development / benchmarking).
        add_executable(u3d_reconstruct tools/u3d_reconstruct.cpp)
        target_link_libraries(u3d_reconstruct PRIVATE u3d_sfm)
    endif()
endif()
