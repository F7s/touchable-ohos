# nx2android HarmonyOS NEXT platform layer (OHOS SDK, arm64-v8a): builds libdmhost.so, the NAPI host the ArkTS
# page loads (XComponent libraryname "dmhost").
#
# The pipeline follows d2r-mobile-main/dm/harmony/native/CMakeLists.txt (MIT, Copyright (c) 2026 jhaizhou):
# native ELF TLS, machine paths kept out of the package, and the platform-neutral Android files reused through
# native/android_shim. P0 starts with the minimal host only; later milestones add the shared sources here:
#   dm/host (main.c compiled with -Dmain=dm_main), dm/gfx/nvn, dm/gfx/tex, dm/android/gfx_vk_*, dm/android/compat.
cmake_minimum_required(VERSION 3.22)
project(dmhost C CXX ASM)

set(DM_HARMONY ${CMAKE_CURRENT_SOURCE_DIR})
set(NX2A ${DM_HARMONY}/../..)          # nx2android-main/nxnative/dm
set(DM ${NX2A})                        # namespace used by d2r's CMake, kept for the later source list

# Native ELF TLS: the default emulated TLS routes every thread_local access through __emutls_get_address, which
# costs about 10% of the translation thread (as measured on d2r); musl already reads TPIDR_EL0.
add_compile_options(-fno-emulated-tls)
# Keep this machine's paths out of the package (as on Android / d2r).
get_filename_component(NX2A_ROOT_ABS ${NX2A}/../.. ABSOLUTE)
add_compile_options(-ffile-prefix-map=${NX2A_ROOT_ABS}=. -ffile-prefix-map=${CMAKE_BINARY_DIR}=build)

set(DM_CFLAGS -O2 -g -D_GNU_SOURCE -Wall -Wno-unused-function -fno-strict-aliasing)

# The Android NDK shim headers (ANativeWindow -> OHNativeWindow, VK_OHOS_surface, Android log -> hilog) are here for
# the next milestone; the P0 host does not include them yet.
add_library(dmhost SHARED ${DM_HARMONY}/napi_ohos.c ${DM_HARMONY}/gfx_surface_ohos.c ${DM_HARMONY}/unzip_fd.c)
target_include_directories(dmhost BEFORE PRIVATE ${DM_HARMONY}/android_shim)
target_compile_definitions(dmhost PRIVATE DM_TARGET_OHOS=1)
target_compile_options(dmhost PRIVATE $<$<COMPILE_LANGUAGE:C>:${DM_CFLAGS} -std=gnu17>)
target_link_libraries(dmhost PRIVATE
    ace_napi.z
    ace_ndk.z
    hilog_ndk.z
    native_window
    native_image
    vulkan
    qos
    dl
    z
    m)
target_link_options(dmhost PRIVATE -Wl,--no-undefined -Wl,-z,nostart-stop-gc -Wl,-Bsymbolic)
