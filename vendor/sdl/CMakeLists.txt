# Tracks the release-3.4.x branch of SDL3. 
# To update, change GIT_TAG to a commit hash from that branch.
# Read the sdl wiki for more info about CMake-configurations: https://wiki.libsdl.org/SDL3/README-cmake
# FIXME: until the next stable release, we'll switch back to the release build

include(FetchContent)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY "https://github.com/libsdl-org/SDL.git"
    GIT_TAG        "53ae9c9370a6cdcbb922b3e3e11addfe806b2f88"
)

set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)
set(SDL_PRESEED                  OFF CACHE BOOL     "" FORCE)
FetchContent_MakeAvailable(SDL3)