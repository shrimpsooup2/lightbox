# lb_import: model and texture importers (Stage 1). Qt-free and GPU-free, like lb_core.
add_library(lb_import STATIC
    importer.cpp
    importer.hpp
    gltf_importer.cpp
    obj_importer.cpp
    fbx_importer.cpp
    mesh_processing.cpp
    mesh_processing.hpp
    scene_builder.cpp
    texture_set.cpp
    formats.hpp)

target_link_libraries(lb_import
    PUBLIC lb_core
    PRIVATE lightbox_cgltf lightbox_tinyobjloader lightbox_ufbx)
lightbox_set_warnings(lb_import)
