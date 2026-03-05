set(SRCS
    exports.h
    event.cpp
    graphics.cpp
    main.cpp
    textures.cpp
    trigger.cpp
    vtypes.h
    resources.rc
    resources.h
    level.cpp
    lightmap.cpp
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
    ${CMAKE_SOURCE_DIR}/vendor/d3d8
)

target_link_libraries(AlpineEditor
    PatchCommon
    Common
    Xlog
    CrashHandlerStub
    shlwapi
)
