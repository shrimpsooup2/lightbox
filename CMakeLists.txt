cmake_minimum_required(VERSION 3.25)

project(LightBox
    VERSION 0.1.0
    DESCRIPTION "Open-source architectural visualization and rendering"
    HOMEPAGE_URL "https://github.com/shrimpsooup2/lightbox"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(LIGHTBOX_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(LIGHTBOX_DEPLOY_QT_ON_BUILD "Run windeployqt after building the app so it runs from the build tree" ON)
option(LIGHTBOX_BUILD_TESTS "Build the unit tests (tests/, no GPU required)" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(LightBoxWarnings)
include(LightBoxDependencies)
include(LightBoxShaders)

add_subdirectory(src/core)
add_subdirectory(src/render)
add_subdirectory(src/import)
add_subdirectory(src/project)
add_subdirectory(src/app)

if(LIGHTBOX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
