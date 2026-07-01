# To update or downgrade SDL3, change the SDL3_VERSION and SDL3_HASH (sha256) variables below. 
# Version histories and sha256 hashes can be found at SDL release pages: https://github.com/libsdl-org/SDL/releases
# Read the sdl wiki for more info about CMake-configurations: https://wiki.libsdl.org/SDL3/README-cmake

set(SDL3_VERSION "3.4.12")
set(SDL3_HASH "f07b958a9ac5020fb7a44cadb957f658b2149c3c8abb4f63145fac9303249db7")

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

# Disable all subsystems except those required for gamepad/joystick input
set(SDL_AUDIO        OFF CACHE BOOL "" FORCE)
set(SDL_VIDEO        OFF CACHE BOOL "" FORCE)
set(SDL_GPU          OFF CACHE BOOL "" FORCE)
set(SDL_RENDER       OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA       OFF CACHE BOOL "" FORCE)
set(SDL_POWER        OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG       OFF CACHE BOOL "" FORCE)
set(SDL_TRAY         OFF CACHE BOOL "" FORCE)
set(SDL_NOTIFICATION OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(SDL3)
