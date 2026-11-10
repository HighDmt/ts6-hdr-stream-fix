cmake_minimum_required(VERSION 3.28)
project(hdrfix VERSION 1.1.3 LANGUAGES C CXX RC)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "hdrfix must be built with MSVC for x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the DLL must not need any MSVC runtime DLLs next to TeamSpeak.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- MinHook ---------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG        c3fcafdc10146beb5919319d0683e44e3c30d537 # v1.3.4
  EXCLUDE_FROM_ALL) # linked statically; keeps MinHook's own install rules out of the release
FetchContent_MakeAvailable(minhook)

# --- Generated files: shaders (compiled at build time, no runtime d3dcompiler) and version.h ---
set(_pf86 "ProgramFiles(x86)")
set(_kits "$ENV{${_pf86}}/Windows Kits/10/bin")
file(GLOB _kit_bins LIST_DIRECTORIES true "${_kits}/10.*/x64")
list(SORT _kit_bins COMPARE NATURAL ORDER DESCENDING)
find_program(FXC_EXE fxc REQUIRED HINTS
  "${_kits}/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
  "$ENV{WindowsSdkVerBinPath}x64"
  ${_kit_bins})

set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/hdr_to_sdr.hlsl)
set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(
  OUTPUT ${GEN_DIR}/hdr_to_sdr_vs.h ${GEN_DIR}/hdr_to_sdr_ps.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN_DIR}
  COMMAND ${FXC_EXE} /nologo /O3 /T vs_4_0 /E VSMain /Vn g_VSMain /Fh ${GEN_DIR}/hdr_to_sdr_vs.h ${SHADER_SRC}
  COMMAND ${FXC_EXE} /nologo /O3 /T ps_4_0 /E PSMain /Vn g_PSMain /Fh ${GEN_DIR}/hdr_to_sdr_ps.h ${SHADER_SRC}
  DEPENDS ${SHADER_SRC}
  COMMENT "Compiling hdr_to_sdr.hlsl"
  VERBATIM)
configure_file(src/version.h.in ${GEN_DIR}/version.h @ONLY)

set(COMMON_DEFS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

# --- The proxy DLL (built as version.dll) -----------------------------------
add_library(hdrfix SHARED
  src/dllmain.cpp
  src/proxy_version.cpp
  src/version.def
  src/version.rc
  src/delay_load.cpp
  src/log_config.cpp
  src/verbose_log.cpp
  src/hooks.cpp
  src/window_fix.cpp
  src/window_capture.cpp
  src/vtable_index.c
  src/duplication_wrapper.cpp
  src/converter.cpp
  src/display_info.cpp
  ${GEN_DIR}/hdr_to_sdr_vs.h
  ${GEN_DIR}/hdr_to_sdr_ps.h)
target_include_directories(hdrfix PRIVATE ${GEN_DIR})
target_compile_definitions(hdrfix PRIVATE ${COMMON_DEFS})
target_compile_options(hdrfix PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:/W4 /permissive->)
# Only kernel32 is imported normally: everything else is delay-loaded (from System32,
# see delay_load.cpp) and only touched in the process that gets hooked.
# kernel32 first, so plain kernel32 functions don't bind to windowsapp's api-set names.
target_link_libraries(hdrfix PRIVATE kernel32 minhook d3d11 dxgi user32 gdi32 dwmapi windowsapp delayimp)
target_link_options(hdrfix PRIVATE /DELAYLOAD:d3d11.dll /DELAYLOAD:dxgi.dll /DELAYLOAD:user32.dll
  /DELAYLOAD:gdi32.dll /DELAYLOAD:dwmapi.dll /DELAYLOAD:oleaut32.dll
  /DELAYLOAD:api-ms-win-core-winrt-l1-1-0.dll /DELAYLOAD:api-ms-win-core-winrt-error-l1-1-1.dll
  /DELAYLOAD:api-ms-win-core-com-l1-1-0.dll)
set_target_properties(hdrfix PROPERTIES
  OUTPUT_NAME version
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/dll"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/lib")
add_custom_command(TARGET hdrfix POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/hdrfix.ini $<TARGET_FILE_DIR:hdrfix>)

# --- Standalone verification program ----------------------------------------
add_executable(hdrtest test/hdrtest.cpp src/display_info.cpp)
target_compile_definitions(hdrtest PRIVATE ${COMMON_DEFS})
target_compile_options(hdrtest PRIVATE /W4 /permissive-)
target_link_libraries(hdrtest PRIVATE d3d11 dxgi windowscodecs ole32 user32 gdi32)
set_target_properties(hdrtest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/test")
add_dependencies(hdrtest hdrfix)

# --- Converter test on WARP (software GPU): runs anywhere, including CI ------
add_executable(convtest test/convtest.cpp src/converter.cpp src/log_config.cpp src/display_info.cpp)
target_include_directories(convtest PRIVATE ${GEN_DIR})
target_compile_definitions(convtest PRIVATE ${COMMON_DEFS})
target_compile_options(convtest PRIVATE /W4 /permissive-)
target_link_libraries(convtest PRIVATE d3d11 dxgi user32)
set_target_properties(convtest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/test")
add_dependencies(convtest hdrfix) # hdrfix generates the shader headers

# --- Release layout: cmake --install <build> --config Release --prefix dist ---
#   dist/                 what users copy into the TeamSpeak folder, plus docs
#   dist/test/hdrtest.exe the verification program (finds the DLL in ..)
install(TARGETS hdrfix RUNTIME DESTINATION .)
install(FILES hdrfix.ini README.md LICENSE THIRD_PARTY_NOTICES.md DESTINATION .)
install(TARGETS hdrtest RUNTIME DESTINATION test)
