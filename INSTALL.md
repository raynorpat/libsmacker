# Compilation and Installation

<http://libsmacker.sourceforge.net>

There are two ways to compile libsmacker for use in your own programs.

## 1. Direct compilation

This method is preferred when you can't (or don't want to) build a system
library. Simply take `smacker.c`, `smacker.h`, and `smk_malloc.h`, add them to
your project, and compile them in as usual.

All external functions (those you, a user, would care about) are contained in
`smacker.h` - so, in your main program, just:

```c
#include "smacker.h"
```

and you're ready to decode `.smk` files.

To ease the process of managing libsmacker compilation, and to keep your source
tree uncluttered, you may wish to use libsmacker as a compiled static library
instead. This allows you to redistribute it, or compile once / separately from
your project, but with the advantage that linking against it does not create a
run-time dependency. See the next section.

## 2. CMake build

libsmacker is distributed with a CMake build. It requires CMake 3.16 or newer
and a C99 compiler.

From the root directory, run:

```sh
cmake -S . -B build
cmake --build build
```

By default this builds both a static and a shared libsmacker library. Build
outputs are written to the `build` directory.

The build is controlled by these CMake options:

| Option | Default | Description |
| --- | --- | --- |
| `SMACKER_BUILD_STATIC` | `ON` | Build the static libsmacker library. |
| `SMACKER_BUILD_SHARED` | `ON` | Build the shared libsmacker library. |
| `SMACKER_BUILD_TOOLS` | `OFF` | Build the `driver` and `smk2avi` example programs. |

At least one of `SMACKER_BUILD_STATIC` or `SMACKER_BUILD_SHARED` must be enabled.

For example, to build only the static library and the example programs:

```sh
cmake -S . -B build \
  -DSMACKER_BUILD_SHARED=OFF \
  -DSMACKER_BUILD_TOOLS=ON
cmake --build build
```

The default configuration is `RelWithDebInfo` for single-configuration
generators such as Makefiles and Ninja. To build with debugging symbols and
assertions enabled, use:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

With multi-configuration generators such as Visual Studio or Xcode, choose the
configuration at build time instead:

```sh
cmake --build build --config Release
```

To change compiler flags, pass them through `CMAKE_C_FLAGS`, for example:

```sh
cmake -S . -B build -DCMAKE_C_FLAGS="-march=i686 -O2 -DNDEBUG"
```

### Using libsmacker from another CMake project

When libsmacker is included with `add_subdirectory`, link against the
`Smacker::smacker` alias:

```cmake
add_subdirectory(path/to/libsmacker)
target_link_libraries(myapp PRIVATE Smacker::smacker)
```

The alias refers to the shared library when `SMACKER_BUILD_SHARED` is enabled,
and to the static library otherwise. The library's include directory is
propagated to consumers automatically.

The build does not currently install a CMake package configuration file, so
`find_package(libsmacker)` is not available. After installation, link against
the installed library directly as described below.

## 3. Installation

The CMake build can install the header and libraries with:

```sh
cmake --install build --prefix /usr/local
```

This copies `smacker.h` to the include directory and the built libraries to the
library directory under the chosen prefix. On Windows, the shared library's DLL
is placed in the binary directory. The `driver` and `smk2avi` example programs
are not installed.

The static library can be compiled directly into an existing codebase:

```sh
cc main.c extra.c libsmacker.a
```

or, after installation, linked as:

```sh
cc main.c -lsmacker
```

The shared library needs to be distributed with your application, which can
save space when several programs use libsmacker.

## Example programs

`driver` and `smk2avi` are example programs that show how to use libsmacker.
They can also test a successful build.

Build them with `-DSMACKER_BUILD_TOOLS=ON`; the executables are written to the
build directory:

```sh
./build/driver file.smk
./build/smk2avi file.smk
```

`driver` takes one `.smk` file and writes frames to a `bmp/` subdirectory and
audio to `out_*.raw` files. `smk2avi` accepts one or more `.smk` files and
converts each to an AVI file.
