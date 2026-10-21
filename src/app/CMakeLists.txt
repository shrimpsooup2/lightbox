# LightBox executable: the Qt Widgets shell. The only target allowed to use Qt.
find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Svg Network)
qt_standard_project_setup()

set(LIGHTBOX_ICON_SIZES 16 24 32 48 64 128 256)
set(branding_files "${PROJECT_SOURCE_DIR}/branding/png/lightbox-logo-dark.png"
                   "${PROJECT_SOURCE_DIR}/branding/png/lightbox-logo.png")
foreach(size IN LISTS LIGHTBOX_ICON_SIZES)
    list(APPEND branding_files "${PROJECT_SOURCE_DIR}/branding/png/lightbox-icon-${size}.png")
endforeach()

qt_add_executable(LightBox
    background.hpp
    coming_soon.cpp
    coming_soon.hpp
    design_gallery.cpp
    design_gallery.hpp
    inspector_panel.cpp
    inspector_panel.hpp
    import_panel.cpp
    import_panel.hpp
    light_panel.cpp
    light_panel.hpp
    main.cpp
    main_window.cpp
    main_window.hpp
    materials_panel.cpp
    materials_panel.hpp
    outliner_panel.cpp
    outliner_panel.hpp
    project_controller.cpp
    project_controller.hpp
    property_widgets.cpp
    property_widgets.hpp
    render_panel.cpp
    render_panel.hpp
    render_screen.cpp
    render_screen.hpp
    scene_tree_model.cpp
    scene_tree_model.hpp
    start_screen.cpp
    start_screen.hpp
    texture_browser.cpp
    texture_browser.hpp
    texture_slot.cpp
    texture_slot.hpp
    theme.cpp
    theme.hpp
    ui_helpers.cpp
    ui_helpers.hpp
    viewport_window.cpp
    viewport_window.hpp
    workspace.cpp
    workspace.hpp)

qt_add_resources(LightBox branding
    PREFIX "/branding"
    BASE "${PROJECT_SOURCE_DIR}/branding/png"
    FILES ${branding_files})

# Inter (OFL-1.1) and Lucide icons (ISC); license texts live next to the files.
file(GLOB ui_fonts CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/*.ttf")
file(GLOB ui_icons CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/*.svg")
qt_add_resources(LightBox ui
    PREFIX "/"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    FILES ${ui_fonts} ${ui_icons})

if(WIN32)
    configure_file(lightbox.rc.in "${CMAKE_CURRENT_BINARY_DIR}/lightbox.rc" @ONLY)
    target_sources(LightBox PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/lightbox.rc")
    # The resource compiler does not report the icon as a dependency; declare it so icon edits relink.
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/lightbox.rc" PROPERTIES
        OBJECT_DEPENDS "${PROJECT_SOURCE_DIR}/branding/lightbox.ico")
endif()

# Console window in Debug (logs on stderr); GUI subsystem otherwise.
set_target_properties(LightBox PROPERTIES
    WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>
    MACOSX_BUNDLE ON)

target_link_libraries(LightBox PRIVATE lb_project lb_import lb_render lb_core
    Qt6::Widgets Qt6::Svg Qt6::Network)
lightbox_set_warnings(LightBox)

if(WIN32 AND LIGHTBOX_DEPLOY_QT_ON_BUILD AND TARGET Qt6::windeployqt)
    add_custom_command(TARGET LightBox POST_BUILD
        COMMAND Qt6::windeployqt --no-translations --no-system-d3d-compiler --no-system-dxc-compiler
                --no-opengl-sw --verbose 0 "$<TARGET_FILE:LightBox>"
        COMMENT "Deploying the Qt runtime next to LightBox.exe"
        VERBATIM)
endif()
