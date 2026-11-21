cmake_minimum_required(VERSION 3.22)
project(ioswine-surface C)

# NDK r28, API 34, arm64 only. Build with:
#   cmake -S src/server -B build/server \
#     -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-34 \
#     -DCMAKE_BUILD_TYPE=Release
# See BUILD.md.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Experimental EGL texture-upload path (server_egl.c). OFF by default:
# it is written but not compiled/tested on device; see server_egl.c.
option(IOSWINE_USE_EGL "Enable experimental EGL presentation path" OFF)

add_library(ioswine-surface SHARED
    server.c
    sysinfo.c
    ui.c
    ../common/protocol.c
    ../common/touch_input.c
    $ENV{ANDROID_NDK_HOME}/sources/android/native_app_glue/android_native_app_glue.c
)

if(IOSWINE_USE_EGL)
    target_sources(ioswine-surface PRIVATE server_egl.c)
    target_compile_definitions(ioswine-surface PRIVATE IOSWINE_USE_EGL=1)
endif()

target_include_directories(ioswine-surface PRIVATE
    ../common
    $ENV{ANDROID_NDK_HOME}/sources/android/native_app_glue
)

target_link_libraries(ioswine-surface
    android
    log
    EGL
    GLESv2
)

# Keep the .so small and free of unused glue symbols.
target_compile_options(ioswine-surface PRIVATE -Wall -Wextra -O2 -fvisibility=hidden)
