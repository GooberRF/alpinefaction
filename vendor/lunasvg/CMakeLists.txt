include(FetchContent)

set(LUNASVG_VERSION 3.5.0)
set(LUNASVG_HASH 1ABF1472EE6C4D19797916E8CC3C2E4B628E0D81178FFAC60BDB0D457E32C690)
set(PLUTOVG_VERSION 1.3.3)
set(PLUTOVG_HASH 2B0D17A6E016F47B86F9C00E2CB82600041B1EA1F7D2A00C2D46AE542CBFED3C)

message(STATUS "Configuring LunaSVG ${LUNASVG_VERSION} with PlutoVG ${PLUTOVG_VERSION}")

# SOURCE_SUBDIR names a directory that does not exist, so FetchContent downloads the sources
# without configuring the upstream projects: theirs pull in find_package(Threads),
# check_include_file, install rules and pkg-config generation that this build has no use for.
# The library is built below from our own target instead (stb pattern).
# PlutoVG is fetched separately even though the lunasvg tarball embeds a copy: the embedded one
# is 1.3.1, which predates the raster/surface allocation hardening in 1.3.3.
FetchContent_Declare(
    lunasvg
    URL https://github.com/sammycage/lunasvg/archive/refs/tags/v${LUNASVG_VERSION}.tar.gz
    URL_HASH SHA256=${LUNASVG_HASH}
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    SOURCE_SUBDIR do-not-configure
)

FetchContent_Declare(
    plutovg
    URL https://github.com/sammycage/plutovg/archive/refs/tags/v${PLUTOVG_VERSION}.tar.gz
    URL_HASH SHA256=${PLUTOVG_HASH}
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    SOURCE_SUBDIR do-not-configure
)

FetchContent_MakeAvailable(lunasvg plutovg)

add_library(lunasvg STATIC
    ${lunasvg_SOURCE_DIR}/source/lunasvg.cpp
    ${lunasvg_SOURCE_DIR}/source/graphics.cpp
    ${lunasvg_SOURCE_DIR}/source/svgelement.cpp
    ${lunasvg_SOURCE_DIR}/source/svggeometryelement.cpp
    ${lunasvg_SOURCE_DIR}/source/svglayoutstate.cpp
    ${lunasvg_SOURCE_DIR}/source/svgpaintelement.cpp
    ${lunasvg_SOURCE_DIR}/source/svgparser.cpp
    ${lunasvg_SOURCE_DIR}/source/svgproperty.cpp
    ${lunasvg_SOURCE_DIR}/source/svgrenderstate.cpp
    ${lunasvg_SOURCE_DIR}/source/svgtextelement.cpp
    ${plutovg_SOURCE_DIR}/source/plutovg-blend.c
    ${plutovg_SOURCE_DIR}/source/plutovg-canvas.c
    ${plutovg_SOURCE_DIR}/source/plutovg-font.c
    ${plutovg_SOURCE_DIR}/source/plutovg-matrix.c
    ${plutovg_SOURCE_DIR}/source/plutovg-paint.c
    ${plutovg_SOURCE_DIR}/source/plutovg-path.c
    ${plutovg_SOURCE_DIR}/source/plutovg-rasterize.c
    ${plutovg_SOURCE_DIR}/source/plutovg-surface.c
    ${plutovg_SOURCE_DIR}/source/plutovg-ft-math.c
    ${plutovg_SOURCE_DIR}/source/plutovg-ft-raster.c
    ${plutovg_SOURCE_DIR}/source/plutovg-ft-stroker.c
)

set_target_properties(lunasvg PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_include_directories(lunasvg PUBLIC
    ${lunasvg_SOURCE_DIR}/include
    ${plutovg_SOURCE_DIR}/include
)

target_include_directories(lunasvg PRIVATE
    ${lunasvg_SOURCE_DIR}/source
    ${plutovg_SOURCE_DIR}/source
)

# *_BUILD_STATIC must be public: it makes LUNASVG_API/PLUTOVG_API expand to nothing, which is
# what consumers of the static library have to see. *_BUILD only affects the library sources.
target_compile_definitions(lunasvg PUBLIC LUNASVG_BUILD_STATIC PLUTOVG_BUILD_STATIC)
target_compile_definitions(lunasvg PRIVATE
    LUNASVG_BUILD
    PLUTOVG_BUILD
    LUNASVG_DISABLE_LOAD_SYSTEM_FONTS
    PLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD
)
