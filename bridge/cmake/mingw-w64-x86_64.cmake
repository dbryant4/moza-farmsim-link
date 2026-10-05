# Cross-compiles the bridge for 64-bit Windows from macOS/Linux with mingw-w64:
#   cmake -S bridge -B bridge/build-win --toolchain cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
