include(FetchContent)

set(FREETYPE_VERSION 2.14.3)
set(FREETYPE_HASH 36BC4F1CC413335368EE656C42AFCA65C5A3987E8768CC28CF11BA775E785A5F)

FetchContent_Declare(
    freetype
    URL https://download.savannah.gnu.org/releases/freetype/freetype-${FREETYPE_VERSION}.tar.xz
    URL_HASH SHA256=${FREETYPE_HASH}
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
)

set(FT_DISABLE_ZLIB ON)
set(FT_DISABLE_BZIP2 ON)
set(FT_DISABLE_BROTLI ON)
set(FT_DISABLE_PNG ON)

FetchContent_MakeAvailable(freetype)
