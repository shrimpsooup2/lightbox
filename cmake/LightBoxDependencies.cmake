# Third-party dependencies, pinned to exact tags. Qt is provided by the system (CMAKE_PREFIX_PATH).
# Policy: see AGENTS.md section 9 before adding anything here, and record the reason in docs/DECISIONS.md.
include(FetchContent)

set(LIGHTBOX_VULKAN_SDK_TAG vulkan-sdk-1.4.363.0)

# Vulkan headers (Apache-2.0 / MIT). Provides the Vulkan::Headers target.
FetchContent_Declare(VulkanHeaders
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        ${LIGHTBOX_VULKAN_SDK_TAG}
    GIT_SHALLOW    TRUE)

# volk meta-loader (MIT). Built by us below; its own CMake would go looking for a Vulkan SDK.
FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG        ${LIGHTBOX_VULKAN_SDK_TAG}
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  lightbox-no-cmake)

# Vulkan Memory Allocator (MIT). Header-only; implementation compiled in src/render/vma.cpp.
FetchContent_Declare(VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  lightbox-no-cmake)

# GLM (MIT). Header-only.
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.3
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  lightbox-no-cmake)

FetchContent_MakeAvailable(VulkanHeaders volk VulkanMemoryAllocator glm)

add_library(lightbox_volk STATIC "${volk_SOURCE_DIR}/volk.c")
target_include_directories(lightbox_volk SYSTEM PUBLIC "${volk_SOURCE_DIR}")
target_link_libraries(lightbox_volk PUBLIC Vulkan::Headers ${CMAKE_DL_LIBS})
target_compile_definitions(lightbox_volk PUBLIC VK_NO_PROTOTYPES)

add_library(lightbox_vma INTERFACE)
target_include_directories(lightbox_vma SYSTEM INTERFACE "${vulkanmemoryallocator_SOURCE_DIR}/include")

add_library(lightbox_glm INTERFACE)
target_include_directories(lightbox_glm SYSTEM INTERFACE "${glm_SOURCE_DIR}")
target_compile_definitions(lightbox_glm INTERFACE GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS)

# Image decoding: stb_image (public domain / MIT). Header-only; implementation in src/core/image.cpp.
# stb has no release tags, so a commit is pinned (D-021).
FetchContent_Declare(stb
    URL https://github.com/nothings/stb/archive/2c980bb59875b0d32144a71867fbdebb2f77cd20.tar.gz
    SOURCE_SUBDIR lightbox-no-cmake)

# Model import (D-021). cgltf: glTF 2.0 / GLB (MIT). tinyobjloader: OBJ/MTL (MIT). ufbx: FBX (MIT or
# public domain). All single-file; implementations compiled in src/import/.
FetchContent_Declare(cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG        v1.15
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  lightbox-no-cmake)
FetchContent_Declare(tinyobjloader
    GIT_REPOSITORY https://github.com/tinyobjloader/tinyobjloader.git
    GIT_TAG        v2.0.0rc13
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  lightbox-no-cmake)
FetchContent_Declare(ufbx
    GIT_REPOSITORY https://github.com/ufbx/ufbx.git
    GIT_TAG        v0.23.1
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  lightbox-no-cmake)

FetchContent_MakeAvailable(stb cgltf tinyobjloader ufbx)

# Project files (D-022): nlohmann/json (MIT, header-only) and miniz (MIT, zip read/write).
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  lightbox-no-cmake)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INSTALL_PROJECT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG        3.1.2
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(nlohmann_json miniz)
add_library(lightbox_json INTERFACE)
target_include_directories(lightbox_json SYSTEM INTERFACE "${nlohmann_json_SOURCE_DIR}/single_include")

add_library(lightbox_stb INTERFACE)
target_include_directories(lightbox_stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")
add_library(lightbox_cgltf INTERFACE)
target_include_directories(lightbox_cgltf SYSTEM INTERFACE "${cgltf_SOURCE_DIR}")
add_library(lightbox_tinyobjloader INTERFACE)
target_include_directories(lightbox_tinyobjloader SYSTEM INTERFACE "${tinyobjloader_SOURCE_DIR}")
# ufbx is a .c file: compiled as its own library so LightBox warnings flags don't apply to it.
add_library(lightbox_ufbx STATIC "${ufbx_SOURCE_DIR}/ufbx.c")
target_include_directories(lightbox_ufbx SYSTEM PUBLIC "${ufbx_SOURCE_DIR}")

# Shader compiler: use glslang from the Vulkan SDK if installed, otherwise build it from source.
find_program(LIGHTBOX_GLSLANG_EXECUTABLE NAMES glslang glslangValidator
    HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")
if(LIGHTBOX_GLSLANG_EXECUTABLE)
    message(STATUS "LightBox: using glslang at ${LIGHTBOX_GLSLANG_EXECUTABLE}")
    set(LIGHTBOX_GLSLANG_COMMAND "${LIGHTBOX_GLSLANG_EXECUTABLE}")
    set(LIGHTBOX_GLSLANG_DEPENDS "")
else()
    message(STATUS "LightBox: no glslang found, building it from source (first configure takes a few minutes)")
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glslang
        GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
        GIT_TAG        ${LIGHTBOX_VULKAN_SDK_TAG}
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(glslang)
    if(MINGW)
        # The compiler runs during the build, when the MinGW runtime DLLs may not be on PATH.
        target_link_options(glslang-standalone PRIVATE -static)
    endif()
    set(LIGHTBOX_GLSLANG_COMMAND "$<TARGET_FILE:glslang-standalone>")
    set(LIGHTBOX_GLSLANG_DEPENDS glslang-standalone)
endif()

# doctest (MIT), unit tests only. Header-only.
if(LIGHTBOX_BUILD_TESTS)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG        v2.5.3
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  lightbox-no-cmake)
    FetchContent_MakeAvailable(doctest)
    add_library(lightbox_doctest INTERFACE)
    target_include_directories(lightbox_doctest SYSTEM INTERFACE "${doctest_SOURCE_DIR}")
endif()
