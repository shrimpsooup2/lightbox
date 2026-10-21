# lb_render: the Vulkan "Live" engine. Depends on lb_core and the Vulkan stack — never on Qt.
add_library(lb_render STATIC
    path_tracer.cpp
    path_tracer.hpp
    ray_scene.hpp
    frame_data.hpp
    gpu_context.cpp
    gpu_context.hpp
    pipelines.cpp
    pipelines.hpp
    presenter.cpp
    renderer.cpp
    renderer.hpp
    renderer_impl.hpp
    scene_sync.cpp
    scene_sync.hpp
    types.hpp
    vma.cpp
    vulkan.hpp)

lightbox_add_shaders(lb_render
    INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    SHADERS
        shaders/fullscreen.vert
        shaders/sky.frag
        shaders/grid.frag
        shaders/mesh.vert
        shaders/mesh.frag
        shaders/shadow.vert
        shaders/overlay.vert
        shaders/overlay.frag
        shaders/composite.frag
        shaders/exposure.comp
        shaders/pathtrace.comp)

target_link_libraries(lb_render
    PUBLIC lb_core Vulkan::Headers
    PRIVATE lightbox_volk lightbox_vma)
target_compile_definitions(lb_render PUBLIC VK_NO_PROTOTYPES)
lightbox_set_warnings(lb_render)

# VMA's implementation is third-party code; keep our warning level off it.
if(MSVC)
    set_source_files_properties(vma.cpp PROPERTIES COMPILE_OPTIONS "/W0")
else()
    set_source_files_properties(vma.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()
