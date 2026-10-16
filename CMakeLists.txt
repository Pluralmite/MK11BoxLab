cmake_minimum_required(VERSION 3.16)
project(MK11BoxLab CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Mortal Kombat 11 is 64-bit: configure for x64.")
endif()

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/external/imgui)
set(MINHOOK_DIR ${CMAKE_SOURCE_DIR}/external/minhook)

add_library(MK11BoxLab SHARED
  src/dllmain.cpp
  src/memory.cpp
  src/config.cpp
  src/game.cpp
  src/boxes.cpp
  src/ui.cpp
  src/log.cpp
  src/mk11.cpp
  src/trace.cpp
  src/buffer.cpp
  src/bot.cpp
  src/movelist.cpp
  src/dinput_block.cpp
  src/screenshot.cpp
  src/probe.cpp
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
  ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
  ${MINHOOK_DIR}/src/buffer.c
  ${MINHOOK_DIR}/src/hook.c
  ${MINHOOK_DIR}/src/trampoline.c
  ${MINHOOK_DIR}/src/hde/hde64.c
)

target_include_directories(MK11BoxLab PRIVATE
  src ${IMGUI_DIR} ${IMGUI_DIR}/backends ${MINHOOK_DIR}/include)

target_compile_definitions(MK11BoxLab PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(MK11BoxLab PRIVATE windowscodecs ole32 uuid dxguid xinput9_1_0 d3d11 dxgi d3dcompiler dwmapi psapi imm32)

# The mod itself is MK11BoxLab_core.dll (hot-reloadable); MK11BoxLab.asi is a small loader that
# Ultimate ASI Loader picks up and that loads / reloads the core.
set_target_properties(MK11BoxLab PROPERTIES PREFIX "" OUTPUT_NAME "MK11BoxLab_core" SUFFIX ".dll")

add_library(MK11BoxLabLoader SHARED src/loader.cpp)
set_target_properties(MK11BoxLabLoader PROPERTIES PREFIX "" OUTPUT_NAME "MK11BoxLab" SUFFIX ".asi")
target_compile_definitions(MK11BoxLabLoader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

if(MINGW)
  target_link_options(MK11BoxLabLoader PRIVATE -static -static-libgcc -static-libstdc++ -s)
  target_compile_options(MK11BoxLabLoader PRIVATE -O2)
  target_link_options(MK11BoxLab PRIVATE -static -static-libgcc -static-libstdc++ -s)
  target_compile_options(MK11BoxLab PRIVATE -O2)
endif()
if(MSVC)
  set_property(TARGET MK11BoxLab PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
