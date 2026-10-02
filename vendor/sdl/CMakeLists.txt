# To update or downgrade SDL3, change the SDL3_VERSION and SDL3_HASH (sha256) variables below.
# Version histories and sha256 hashes can be found at SDL release pages: https://github.com/libsdl-org/SDL/releases
# Read the sdl wiki for more info about CMake-configurations: https://wiki.libsdl.org/SDL3/README-cmake

set(SDL3_VERSION "3.4.18")
set(SDL3_HASH "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3")

include(FetchContent)
FetchContent_Declare(
    SDL3
    URL "https://github.com/libsdl-org/SDL/releases/download/release-${SDL3_VERSION}/SDL3-${SDL3_VERSION}.tar.gz"
    URL_HASH SHA256=${SDL3_HASH}
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
)

set(SDL_STATIC      ON  CACHE BOOL "" FORCE)
set(SDL_SHARED      OFF CACHE BOOL "" FORCE)
set(SDL_TEST        OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)

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
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE) # surprisingly enough, it's not needed for gamepads!

FetchContent_MakeAvailable(SDL3)
