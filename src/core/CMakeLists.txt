# lb_core: platform-neutral foundation. Depends on the standard library, GLM and stb_image (private)
# only — no Qt, no Vulkan.
add_library(lb_core STATIC
    bvh.cpp
    bvh.hpp
    camera.cpp
    camera.hpp
    color.cpp
    color.hpp
    gizmo.cpp
    gizmo.hpp
    environment_map.cpp
    environment_map.hpp
    image.cpp
    image.hpp
    log.cpp
    log.hpp
    raycast.cpp
    raycast.hpp
    mesh.hpp
    overlay.hpp
    placement.cpp
    placement.hpp
    primitives.cpp
    primitives.hpp
    sample_scene.cpp
    sample_scene.hpp
    solar.cpp
    solar.hpp
    scene/environment.cpp
    scene/environment.hpp
    scene/ids.hpp
    scene/light.hpp
    scene/look.hpp
    scene/material.hpp
    scene/scene.cpp
    scene/scene.hpp
    scene/selection.cpp
    scene/selection.hpp
    scene/transform.cpp
    scene/transform.hpp)

target_include_directories(lb_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(lb_core PUBLIC lightbox_glm PRIVATE lightbox_stb)
target_compile_definitions(lb_core PUBLIC LIGHTBOX_VERSION="${PROJECT_VERSION}")
target_compile_features(lb_core PUBLIC cxx_std_20)
lightbox_set_warnings(lb_core)
