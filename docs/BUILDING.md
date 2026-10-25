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

Source layout
-------------

The Direct3D 11 renderer core is in `common/include/common/gr_d3d11` and `common/src/gr_d3d11`. It is a CMake
`INTERFACE` target (`GrD3D11Core`): each DLL that links it compiles its sources against its own
`<gr_d3d11_engine.h>`, which declares the engine types and binds the functions listed in
`common/include/common/gr_d3d11/gr_d3d11_bindings.h`. The game's is in `game_patch/graphics/d3d11/engine`,
RED's in `editor_patch/graphics/d3d11/engine`. The target also builds the pixel format helpers in
`common/src/bitmap` the same way. RED's renderer and hook table are in `editor_patch/graphics/d3d11`, next to its
engine folder; editor code that needs more of the renderer than RED's public gr functions goes through
`editor_patch/graphics/editor_gr.h`, which serves both of RED's renderers.
