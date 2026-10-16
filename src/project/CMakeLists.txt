# lb_project: .lbox project files (0.6, D-022). Qt-free and GPU-free, like lb_core.
add_library(lb_project STATIC
    project_file.cpp
    project_file.hpp)

target_link_libraries(lb_project
    PUBLIC lb_core
    PRIVATE lightbox_json miniz)
lightbox_set_warnings(lb_project)
