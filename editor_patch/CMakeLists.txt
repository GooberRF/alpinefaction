set(SRCS
    exports.h
    alpine_color_picker.cpp
    alpine_color_picker.h
    alpine_obj.cpp
    alpine_obj.h
    alpine_spinner.cpp
    alpine_spinner.h
    brush_import.cpp
    brush_import.h
    event.cpp
    file_dialogs.cpp
    file_dialogs.h
    geometry.cpp
    graphics.cpp
    headless_bake.cpp
    headless_bake.h
    mesh_export.cpp
    main.cpp
    mesh.cpp
    mesh.h
    mesh_browser.cpp
    mesh_browser.h
    note.cpp
    note.h
    corona.cpp
    corona.h
    bag.cpp
    bag.h
    weather_region.cpp
    weather_region.h
    projection_camera.cpp
    projection_camera.h
    rope_emitter.cpp
    rope_emitter.h
    textures.cpp
    bitmap_loaders.cpp
    bitmap_loaders.h
    meshes.cpp
    meshes.h
    tbl.cpp
    tbl.h
    trigger.cpp
    vtypes.h
    resources.rc
    resources.h
    level.cpp
    lightmap.cpp
    lightmap_mesh_occluders.cpp
    lightmap_mesh_occluders.h
)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SRCS})

add_library(AlpineEditor SHARED ${SRCS})
target_compile_features(AlpineEditor PUBLIC cxx_std_20)
set_target_properties(AlpineEditor PROPERTIES
    PREFIX ""
    CXX_EXTENSIONS NO
)
enable_warnings(AlpineEditor)
setup_debug_info(AlpineEditor)

target_compile_definitions(AlpineEditor PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX BUILD_DLL)

target_include_directories(AlpineEditor PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor
    ${CMAKE_SOURCE_DIR}/vendor/d3d8
    ${CMAKE_SOURCE_DIR}/vendor/stb
)

target_link_libraries(AlpineEditor
    PatchCommon
    Common
    Xlog
    CrashHandlerStub
    comctl32
    shlwapi
    ole32
    shell32
    uuid
    stb_image
)
