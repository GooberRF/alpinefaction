# SDL3 built from source via FetchContent:
# To upgrade, update SDL3_VERSION to the desired release tag.
# See: https://wiki.libsdl.org/SDL3/README-cmake
set(SDL3_VERSION "3.4.2")

include(FetchContent)
FetchContent_Declare(
    SDL3
    URL "https://github.com/libsdl-org/SDL/releases/download/release-${SDL3_VERSION}/SDL3-${SDL3_VERSION}.tar.gz"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3)
