# Tracks the release-3.4.x branch of SDL3. 
# To update, change GIT_TAG to a commit hash from that branch.
# Read the sdl wiki for more info about CMake-configurations: https://wiki.libsdl.org/SDL3/README-cmake
# FIXME: until the next stable release, we'll switch back to the release build

include(FetchContent)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY "https://github.com/libsdl-org/SDL.git"
    GIT_TAG        "eb8caad5a4c7d89eddc4c63143cadc389359d133"
)

set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)
set(SDL_PRESEED                  OFF CACHE BOOL     "" FORCE)
FetchContent_MakeAvailable(SDL3)