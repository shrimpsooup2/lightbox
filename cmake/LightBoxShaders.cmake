# lightbox_add_shaders(<target> INCLUDE_DIR <dir> SHADERS <file>...)
#
# Compiles GLSL sources to SPIR-V at build time and embeds them as C arrays. For `mesh.vert` the generated
# header is `shaders/mesh.vert.h` (on the target's include path) declaring `lb_spv_mesh_vert[]`.
# Every *.glsl file in INCLUDE_DIR is treated as a dependency of every shader.
function(lightbox_add_shaders target)
    cmake_parse_arguments(ARG "" "INCLUDE_DIR" "SHADERS" ${ARGN})
    set(out_root "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(out_dir "${out_root}/shaders")
    file(MAKE_DIRECTORY "${out_dir}")
    file(GLOB shared_includes CONFIGURE_DEPENDS "${ARG_INCLUDE_DIR}/*.glsl")

    set(outputs "")
    foreach(shader IN LISTS ARG_SHADERS)
        get_filename_component(src "${shader}" ABSOLUTE)
        get_filename_component(name "${shader}" NAME)
        string(REPLACE "." "_" symbol "lb_spv_${name}")
        set(out "${out_dir}/${name}.h")
        add_custom_command(
            OUTPUT "${out}"
            COMMAND ${LIGHTBOX_GLSLANG_COMMAND} -V --target-env vulkan1.3 -I${ARG_INCLUDE_DIR}
                    --vn ${symbol} -o "${out}" "${src}"
            DEPENDS "${src}" ${shared_includes} ${LIGHTBOX_GLSLANG_DEPENDS}
            COMMENT "Compiling shader ${name}"
            VERBATIM)
        list(APPEND outputs "${out}")
    endforeach()

    target_sources(${target} PRIVATE ${outputs})
    target_include_directories(${target} PRIVATE "${out_root}")
endfunction()
