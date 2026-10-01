cmake_minimum_required(VERSION 3.20)

# Conker BFD: Reloaded -- the program (Windows with MSVC, Linux with GCC or Clang).
# Dependencies (C:\ConkerRecompWin\setup_deps.bat): N64ModernRuntime, rt64 and
# RecompFrontend at the versions CBFD-Recompiled uses, with its patches (patches/).
# The recompiled game code is recomp/out.
# Build setup adapted from CBFD-Recompiled's host/CMakeLists.txt (MIT, Sean Ciaschi).

project(ConkerBFDReloaded C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
if (NOT MSVC)
    # Like MSVC: the game code reads memory through different types and relies on numbers wrapping.
    add_compile_options(-fno-strict-aliasing -fwrapv)
endif()

# The version: shown in the launcher and in the exe's Properties. A suffix (e.g. "-beta") marks test builds.
set(CONKER_VERSION_MAJOR 1)
set(CONKER_VERSION_MINOR 1)
set(CONKER_VERSION_PATCH 2)
set(CONKER_VERSION_SUFFIX "")
set(CONKER_VERSION_STRING "${CONKER_VERSION_MAJOR}.${CONKER_VERSION_MINOR}.${CONKER_VERSION_PATCH}${CONKER_VERSION_SUFFIX}")
configure_file(src/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h" @ONLY)
if (WIN32)
    configure_file(icon/conker.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/conker.rc" @ONLY)
endif()

set(DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../deps" CACHE PATH "Runtime, RT64 and RecompFrontend")
set(RECOMP_OUT "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/out" CACHE PATH "N64Recomp output")
set(RUNTIME_DIR "${DEPS_DIR}/N64ModernRuntime")
set(RT64_DIR "${DEPS_DIR}/rt64")

add_subdirectory("${RUNTIME_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/N64ModernRuntime")
if (MSVC)
    # MSVC hits an internal compiler error optimising librecomp's flash.cpp.
    set_source_files_properties("${RUNTIME_DIR}/librecomp/src/flash.cpp"
        DIRECTORY "${RUNTIME_DIR}/librecomp" PROPERTIES COMPILE_OPTIONS "/Od")
endif()

# The recompiled game code.
file(GLOB RECOMPILED_FUNCS CONFIGURE_DEPENDS "${RECOMP_OUT}/funcs_*.c")
add_library(RecompiledFuncs STATIC ${RECOMPILED_FUNCS} "${RECOMP_OUT}/segments.c")
target_include_directories(RecompiledFuncs PRIVATE "${RUNTIME_DIR}/N64Recomp/include" "${RECOMP_OUT}")
# setjmp.h: recomp/conker.toml's hooks on the script interpreter.
if (MSVC)
    target_compile_options(RecompiledFuncs PRIVATE /W0 /bigobj /FIsetjmp.h /FIstdio.h)
else()
    # (GCC 14+ refuses calls to undeclared functions by default; the runtime provides them.)
    target_compile_options(RecompiledFuncs PRIVATE -w "SHELL:-include setjmp.h" "SHELL:-include stdio.h"
        -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types)
endif()

add_executable(ConkerBFDReloaded
    src/main.cpp
    src/segments.cpp
    src/libultra_extras.cpp
    src/frontend.cpp
    src/settings.cpp
    src/sound.cpp
    src/widescreen.cpp
    src/render_fixes.cpp
    src/free_camera.cpp
    src/pause_background.cpp
    src/fps_counter.cpp
    src/pad_mappings.cpp
    src/field_of_view.cpp
    src/crosshair.cpp
    src/qol.cpp
    src/cash_hud.cpp
    src/look_aim.cpp
    src/rumble.cpp
    src/interpolation.cpp
    src/scene_fixes.cpp
    src/sound_mix.cpp
    src/testing.cpp
    "${RECOMP_OUT}/rsp/audio_ucode.cpp"
)
if (WIN32)
    # The exe's icon and version details.
    target_sources(ConkerBFDReloaded PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/conker.rc")
endif()
if (MSVC)
    set_source_files_properties("${RECOMP_OUT}/rsp/audio_ucode.cpp" PROPERTIES COMPILE_OPTIONS "/W0;/bigobj")
else()
    # The audio microcode's vector maths uses SSE4.1.
    set_source_files_properties("${RECOMP_OUT}/rsp/audio_ucode.cpp" PROPERTIES COMPILE_OPTIONS "-w;-msse4.1")
endif()
target_include_directories(ConkerBFDReloaded PRIVATE "${RUNTIME_DIR}/N64Recomp/include" "${RECOMP_OUT}"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${RUNTIME_DIR}/thirdparty/xxHash")
target_link_libraries(ConkerBFDReloaded PRIVATE RecompiledFuncs librecomp ultramodern)

set(RT64_STATIC ON CACHE BOOL "" FORCE)
if (NOT WIN32)
    # On Linux, RT64 (Vulkan) draws into the SDL window, which RecompFrontend expects.
    set(RT64_SDL_WINDOW_VULKAN ON CACHE BOOL "" FORCE)
endif()
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
add_subdirectory("${RT64_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/rt64")

# RecompFrontend: launcher and menus (recompui) and input (recompinput).
if (WIN32)
    set(sdl2_SOURCE_DIR "${RT64_DIR}/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3")
else()
    find_package(SDL2 REQUIRED)
endif()
set(RECOMP_FRONTEND_N64MODERNRUNTIME_PATH "${RUNTIME_DIR}")
set(RECOMP_FRONTEND_RT64_PATH "${RT64_DIR}")
# recompui compiles its shaders with rt64's helpers, which read these from rt64's scope.
set(DXC_COMMON_OPTS "-I${RT64_DIR}/src")
set(DXC_DXIL_OPTS "-Wno-ignored-attributes")
set(DXC_SPV_OPTS "-spirv" "-fspv-target-env=vulkan1.0" "-fvk-use-dx-layout")
set(DXC_PS_OPTS "${DXC_COMMON_OPTS}" "-E" "PSMain" "-T ps_6_0" "-D DYNAMIC_RENDER_PARAMS")
set(DXC_VS_OPTS "${DXC_COMMON_OPTS}" "-E" "VSMain" "-T vs_6_0" "-D DYNAMIC_RENDER_PARAMS" "-fvk-invert-y")
set(DXC_ARCH x64)
if (WIN32)
    set(DXC "${RT64_DIR}/src/contrib/dxc/bin/x64/dxc.exe")
else()
    set(DXC "LD_LIBRARY_PATH=${RT64_DIR}/src/contrib/dxc/lib/x64" "${RT64_DIR}/src/contrib/dxc/bin/x64/dxc-linux")
endif()
if (NOT WIN32)
    # (RT64 sets these for itself; RecompFrontend and this program include its window types too.)
    add_compile_definitions(PLUME_SDL_VULKAN_ENABLED RT64_SDL_WINDOW_VULKAN)
endif()
add_subdirectory("${DEPS_DIR}/RecompFrontend" "${CMAKE_CURRENT_BINARY_DIR}/RecompFrontend")
if (MSVC)
    # Their GCC-style warning flags are errors for cl.
    set_property(TARGET recompui recompinput PROPERTY COMPILE_OPTIONS "")
endif()
target_link_libraries(ConkerBFDReloaded PRIVATE recompui recompinput nfd rt64)

# CONKER_RT64: CBFD-Recompiled's switch for its RT64 (window) build, which is the only one we have.
target_compile_definitions(ConkerBFDReloaded PRIVATE HLSL_CPU NOMINMAX CONKER_RT64)
target_include_directories(ConkerBFDReloaded PRIVATE
    "${RT64_DIR}/src"
    "${RT64_DIR}/src/contrib"
    "${RT64_DIR}/src/contrib/plume"
    "${RT64_DIR}/src/contrib/imgui"
    "${RT64_DIR}/src/contrib/hlslpp/include"
    "${RT64_DIR}/src/contrib/mupen64plus-core/src/api"
    "${RT64_DIR}/src/contrib/nativefiledialog-extended/src/include"
    "${RT64_DIR}/src/contrib/dxc/inc"
)
if (WIN32)
    target_include_directories(ConkerBFDReloaded PRIVATE "${sdl2_SOURCE_DIR}/include")
    target_link_directories(ConkerBFDReloaded PRIVATE "${sdl2_SOURCE_DIR}/lib/x64")
    target_link_libraries(ConkerBFDReloaded PRIVATE SDL2 DbgHelp)
    # Mods patch recompiled functions' machine code, so identical ones must stay separate.
    target_link_options(ConkerBFDReloaded PRIVATE /OPT:NOICF)
    # A windowed program: no terminal opens with it (output still goes wherever it's
    # redirected, e.g. the test scripts' log).
    target_link_options(ConkerBFDReloaded PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)

    # Next to the exe: menu assets, SDL2, and DXC (RT64 loads it at startup).
    add_custom_command(TARGET ConkerBFDReloaded POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:ConkerBFDReloaded>/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll"
            "${RT64_DIR}/src/contrib/dxc/bin/x64/dxcompiler.dll"
            "${RT64_DIR}/src/contrib/dxc/bin/x64/dxil.dll"
            "$<TARGET_FILE_DIR:ConkerBFDReloaded>")
else()
    target_link_libraries(ConkerBFDReloaded PRIVATE SDL2::SDL2 pthread dl)
    # Runs on older Linux systems than the build machine: the C++ library inside the program, and
    # the newest glibc functions sent to older versions (src/linux_glibc_compat.c).
    target_sources(ConkerBFDReloaded PRIVATE src/linux_glibc_compat.c)
    target_link_options(ConkerBFDReloaded PRIVATE -static-libstdc++ -static-libgcc)
    foreach(f acosf asinf atan2f fmodf hypotf hypot sqrtf remainderf arc4random __isoc23_strtol __isoc23_strtoll __isoc23_strtoul __isoc23_strtoull __isoc23_sscanf)
        target_link_options(ConkerBFDReloaded PRIVATE "-Wl,--wrap=${f}")
    endforeach()
    # The shader compiler's libraries sit next to the program.
    set_target_properties(ConkerBFDReloaded PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")

    # Next to the program: menu assets and DXC (RT64 loads it at startup).
    add_custom_command(TARGET ConkerBFDReloaded POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:ConkerBFDReloaded>/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RT64_DIR}/src/contrib/dxc/lib/x64/libdxcompiler.so"
            "${RT64_DIR}/src/contrib/dxc/lib/x64/libdxil.so"
            "$<TARGET_FILE_DIR:ConkerBFDReloaded>")
endif()
