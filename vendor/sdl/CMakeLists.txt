# To update or downgrade SDL3, change the SDL3_VERSION and SDL3_HASH (sha256) variables below. 
# Version histories and sha256 hashes can be found at SDL release pages: https://github.com/libsdl-org/SDL/releases
# Read the sdl wiki for more info about CMake-configurations: https://wiki.libsdl.org/SDL3/README-cmake

set(SDL3_VERSION "3.4.6")
set(SDL3_HASH "96bdd3f0c442105c87c287d213858b2df8207c87d54991dd589078c3746cb386")

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