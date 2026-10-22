# Unit tests: doctest, no GPU and no Qt required. Run with `ctest --preset <preset>` or the exe.
add_executable(lb_tests
    main.cpp
    core/bvh_tests.cpp
    core/camera_lens_tests.cpp
    core/camera_tests.cpp
    core/environment_map_tests.cpp
    core/environment_tests.cpp
    core/gizmo_tests.cpp
    core/image_tests.cpp
    core/placement_tests.cpp
    core/raycast_tests.cpp
    core/scene_tests.cpp
    core/selection_tests.cpp
    core/similar_tests.cpp
    core/solar_tests.cpp
    import/import_tests.cpp
    import/texture_set_tests.cpp
    project/project_file_tests.cpp
    render/ray_scene_tests.cpp
    render/scene_sync_tests.cpp)

target_link_libraries(lb_tests PRIVATE lb_project lb_import lb_core lb_render lightbox_doctest miniz)
lightbox_set_warnings(lb_tests)
if(MINGW)
    # Runs without the MinGW runtime DLLs on PATH.
    target_link_options(lb_tests PRIVATE -static)
endif()

add_test(NAME lb_tests COMMAND lb_tests)
