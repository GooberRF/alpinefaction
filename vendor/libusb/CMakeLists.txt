# libusb-cmake: https://github.com/libusb/libusb-cmake

set(LIBUSB_VERSION "v1.0.30-0")
set(LIBUSB_HASH "c8143ceba42b7b93d484ccd09db7be261bb9b15f61dae5fdc605a01a6913bada")

include(FetchContent)
FetchContent_Declare(
    libusb_cmake
    URL "https://github.com/libusb/libusb-cmake/archive/refs/tags/${LIBUSB_VERSION}.tar.gz"
    URL_HASH SHA256=${LIBUSB_HASH}
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
)

set(LIBUSB_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(LIBUSB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIBUSB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(LIBUSB_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(libusb_cmake)
