Building Alpine Faction
=======================

Windows
-------

On Windows use CMake GUI to generate project files for your favorite IDE.
Supports only Win32 platform. If you are using Visual Studio 2019+ on x86_64 Windows host
CMake by default selects Win64 target platform. Please change it manually to Win32.

Linux
-----

Building on Ubuntu 24.04 based distribution is recommended.
Building on other Linux distributions should be possible but may require different/additional steps that are not
covered by this instruction.

Make sure you have all the needed tools:

* mingw-w64
* cmake
* make

To install them run:

```
sudo apt-get install mingw-w64 cmake make
```

You can use Ninja instead of Make to speed up build.

Checkout source code repository:

```
git clone https://github.com/GooberRF/alpinefaction.git
```

Create `build` directory and generate makefiles (you can use a different directory):

```
cd alpinefaction
mkdir build
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/mingw-ubuntu.cmake -DCMAKE_BUILD_TYPE=Release
```

Compiling the shaders and `alpinefaction.vpp` runs the shader compiler under Wine (vkd3d-shader). Add
`-DALPINE_BUILD_PACKFILE=OFF` to skip them and build only the binaries; take `alpinefaction.vpp` from a Windows build.

Start build:

```
make -j$(nproc)
```

After build is finished you will find binaries in `build/Release/bin` subdirectory.

To update your custom builds run:

```
cd alpinefaction/build
git pull
make -j$(nproc)
```
