# DevEco 构建入口：编 native/ 下的鸿蒙平台层，产出 libdmhost.so（NAPI）。
# P0 只含最小宿主（napi_ohos.c）；后续里程碑在这里加 dm/host、gfx/nvn、gfx/tex、dm/android/gfx_vk_* 的源列表。
# DevEco build entry: builds the HarmonyOS platform layer under native/ into libdmhost.so (NAPI).
# P0 compiles the minimal host only (napi_ohos.c); later milestones extend the source list here (dm/host, gfx/nvn,
# gfx/tex, dm/android/gfx_vk_*), following d2r-mobile-main/dm/harmony/native/CMakeLists.txt (MIT).
cmake_minimum_required(VERSION 3.22)
project(dmhost_entry C)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../../native ${CMAKE_CURRENT_BINARY_DIR}/native)
