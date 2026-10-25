set(SRCS
    include/common/HttpRequest.h
    include/common/rfproto.h
    include/common/rope_curve.h
    include/common/alpine_mesh_scale.h
    include/common/alpine_camera_far_clip.h
    include/common/alpine_dir_light.h
    include/common/alpine_gas_region.h
    include/common/ComPtr.h
    include/common/afd_format.h
    include/common/rfl_chunk_reader.h
    include/common/tlv.h
    include/common/scope_guard.h
    include/common/vehicle_meshes.h
    include/common/vehicle_orient.h
    include/common/config/BuildConfig.h
    include/common/config/CfgVar.h
    include/common/config/GameConfig.h
    include/common/config/AlpineCoreConfig.h
    include/common/config/RegKey.h
    include/common/error/error-utils.h
    include/common/error/Exception.h
    include/common/error/d3d-error.h
    include/common/error/Win32Error.h
    include/common/utils/enum-bitwise-operators.h
    include/common/utils/iterable-utils.h
    include/common/utils/list-utils.h
    include/common/utils/mem-pool.h
    include/common/utils/os-utils.h
    include/common/utils/perf-utils.h
    include/common/utils/string-utils.h
    include/common/utils/bool-utils.h
    include/common/utils/color-utils.h
    include/common/bitmap/formats.h
    include/common/bitmap/tga.h
    include/common/lightmap/alpine_lightmap.h
    include/common/lightmap/alpine_lightmap_decode.h
    include/common/lightmap/alpine_lightmap_reader.h
    include/common/lightmap/af_lightmap_runtime.h
    include/common/lightmap/af_lightmap_surface.h
    include/common/lighting/alpine_lighting.h
    include/common/lighting/alpine_sun_state.h
    include/common/terrain/alpine_terrain.h
    include/common/terrain/alpine_terrain_reader.h
    include/common/terrain/alpine_terrain_runtime.h
    include/common/terrain/alpine_terrain_decorations_runtime.h
    include/common/atx/spec.h
    include/common/atx/parse.h
    include/common/version/version.h
    src/HttpRequest.cpp
    src/config/GameConfig.cpp
    src/config/AlpineCoreConfig.cpp
    src/error/d3d-error.cpp
    src/utils/os-utils.cpp
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SRCS})

add_library(Common STATIC ${SRCS})

target_compile_features(Common PUBLIC cxx_std_23)
set_target_properties(Common PROPERTIES CXX_EXTENSIONS NO)
enable_warnings(Common)

target_include_directories(Common PRIVATE
    include/common
    ${CMAKE_SOURCE_DIR}/vendor/d3d8
)

# Make sure Windows min/max macro don't conflict with std::min/std::max
target_compile_definitions(Common PUBLIC NOMINMAX)

target_include_directories(Common PUBLIC
    include
    ${CMAKE_SOURCE_DIR}/vendor
    ${CMAKE_SOURCE_DIR}/vendor/dds
)

target_link_libraries(Common
    shlwapi
    wininet
    version
    Xlog
)

# Shared Direct3D 11 renderer core. Each consumer compiles these sources against its own
# engine shim, <gr_d3d11_engine.h>, which it puts on its include path.
set(GR_D3D11_CORE_SRCS
    include/common/bitmap/bm_format.h
    include/common/bitmap/fmt_conv_templates.h
    include/common/gr_d3d11/gr_d3d11_af_lightmap.h
    include/common/gr_d3d11/gr_d3d11_bindings.h
    include/common/gr_d3d11/gr_d3d11_buffer.h
    include/common/gr_d3d11/gr_d3d11_caustics.h
    include/common/gr_d3d11/gr_d3d11_context.h
    include/common/gr_d3d11/gr_d3d11_decoration.h
    include/common/gr_d3d11/gr_d3d11_dynamic_geometry.h
    include/common/gr_d3d11/gr_d3d11_gamma.h
    include/common/gr_d3d11/gr_d3d11_liquid.h
    include/common/gr_d3d11/gr_d3d11_mesh.h
    include/common/gr_d3d11/gr_d3d11_particle.h
    include/common/gr_d3d11/gr_d3d11_shader.h
    include/common/gr_d3d11/gr_d3d11_solid.h
    include/common/gr_d3d11/gr_d3d11_state.h
    include/common/gr_d3d11/gr_d3d11_terrain.h
    include/common/gr_d3d11/gr_d3d11_texture.h
    include/common/gr_d3d11/gr_d3d11_transform.h
    include/common/gr_d3d11/gr_d3d11_util.h
    include/common/gr_d3d11/gr_d3d11_vertex.h
    include/common/gr_d3d11/gr_d3d11_vfx.h
    include/common/gr_d3d11/gr_d3d11_weather.h
    src/gr_d3d11/gr_d3d11_af_lightmap.cpp
    src/gr_d3d11/gr_d3d11_caustics.cpp
    src/gr_d3d11/gr_d3d11_context.cpp
    src/gr_d3d11/gr_d3d11_decoration.cpp
    src/gr_d3d11/gr_d3d11_dynamic_geometry.cpp
    src/gr_d3d11/gr_d3d11_error.cpp
    src/gr_d3d11/gr_d3d11_gamma.cpp
    src/gr_d3d11/gr_d3d11_liquid.cpp
    src/gr_d3d11/gr_d3d11_mesh.cpp
    src/gr_d3d11/gr_d3d11_particle.cpp
    src/gr_d3d11/gr_d3d11_shader.cpp
    src/gr_d3d11/gr_d3d11_solid.cpp
    src/gr_d3d11/gr_d3d11_state.cpp
    src/gr_d3d11/gr_d3d11_terrain.cpp
    src/gr_d3d11/gr_d3d11_texture.cpp
    src/gr_d3d11/gr_d3d11_vfx.cpp
    src/bitmap/bm_format.cpp
)
list(TRANSFORM GR_D3D11_CORE_SRCS PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)

add_library(GrD3D11Core INTERFACE)
target_sources(GrD3D11Core INTERFACE ${GR_D3D11_CORE_SRCS})
target_link_libraries(GrD3D11Core INTERFACE Common)
