set(U3D_TEST_SOURCES TestMain.cpp test_splat.cpp)
set(U3D_TEST_LIBS u3d_splat)
set(U3D_TEST_NAMES splat_gradients splat_projection ssim_loss training_recovers ply_round_trip crop)

if(U3D_WITH_SFM)
    list(APPEND U3D_TEST_SOURCES test_sfm.cpp test_pipeline.cpp)
    list(APPEND U3D_TEST_LIBS u3d_sfm)
    list(APPEND U3D_TEST_NAMES bundle_adjustment incremental_sfm pipeline_with)
endif()

add_executable(u3d_tests ${U3D_TEST_SOURCES})
target_link_libraries(u3d_tests PRIVATE ${U3D_TEST_LIBS})

foreach(name IN LISTS U3D_TEST_NAMES)
    add_test(NAME ${name} COMMAND u3d_tests ${name} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(${name} PROPERTIES TIMEOUT 600)
endforeach()
