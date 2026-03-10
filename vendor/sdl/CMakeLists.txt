add_library(SDL3::SDL3-static STATIC IMPORTED GLOBAL)
set_target_properties(SDL3::SDL3-static PROPERTIES
    IMPORTED_LOCATION_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/lib/SDL3-static.lib"
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/lib/SDL3-static.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/include"
    INTERFACE_LINK_LIBRARIES "user32;gdi32;winmm;imm32;ole32;oleaut32;shell32;version;uuid;dinput8;dxguid;setupapi;cfgmgr32"
)
