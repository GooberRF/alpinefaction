# SDL3 built from source via FetchContent:
# To upgrade, update SDL3_VERSION and SDL3_HASH to match the desired release tag.
# Hash can be found at: https://github.com/libsdl-org/SDL/releases
# See: https://wiki.libsdl.org/SDL3/README-cmake
set(SDL3_VERSION "3.4.2")
set(SDL3_HASH "ef39a2e3f9a8a78296c40da701967dd1b0d0d6e267e483863ce70f8a03b4050c")

include(FetchContent)
FetchContent_Declare(
    SDL3
    URL "https://github.com/libsdl-org/SDL/releases/download/release-${SDL3_VERSION}/SDL3-${SDL3_VERSION}.tar.gz"
    URL_HASH SHA256=${SDL3_HASH}
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3)