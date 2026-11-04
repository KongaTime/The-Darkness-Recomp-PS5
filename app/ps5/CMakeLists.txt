# The Darkness on PS5 - the title's build.
#
# A separate project from the repository's root CMakeLists.txt, which is the
# Windows build: this one is configured with the PS5 SDK's toolchain
# (toolchain/prospero.cmake) and compiles the AOT output, the kernel and file
# layers, the loader and the console backends. tools/build.sh drives it.
#
# The target links the translated guest, the kernel and file layers, the
# engine's capture layer (render_trace.cpp's hooks and simple_mesh.cpp's
# decoders and frame queue) and the console's Vulkan display, and presents the
# frames the guest publishes in title_main.cpp.
cmake_minimum_required(VERSION 3.20)
project(darkness_ps5 CXX)
option(DARK_PS5_THIN_LTO "Optimize guest call boundaries after resolving native overrides" OFF)
option(DARK_PS5_NATIVE_COMMAND_EMISSION "Experimental exact native vector-packet writer" OFF)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(DARK_GENERATED_DIR "${ROOT}/build_native/generated" CACHE PATH "verified AOT output")
set(DARK_SIMDE_DIR "${ROOT}/refs/UnleashedRecomp/tools/XenonRecomp/thirdparty/simde"
    CACHE PATH "the vector helpers the AOT code includes")
# The console's link layout: separate PT_LOAD segments with page-aligned
# boundaries, which the native tool needs to place the process parameters.
set(DARK_PS5_LAYOUT "${CMAKE_CURRENT_SOURCE_DIR}/ps5-pie.ld")

# Vulkan's headers, from the Mesa tree RADV was built from: the driver and the
# headers a title compiles against should be the same revision. The SDK ships
# none, and the console has no loader to take them from.
set(VULKAN_HEADERS "${ROOT}/../PS5_Mesa/include" CACHE PATH "Vulkan headers matching the RADV build")
if(NOT EXISTS "${VULKAN_HEADERS}/vulkan/vulkan.h")
    message(FATAL_ERROR "No Vulkan headers at ${VULKAN_HEADERS}/vulkan/vulkan.h (set VULKAN_HEADERS)")
endif()

if(NOT EXISTS "${DARK_GENERATED_DIR}/sources.cmake")
    message(FATAL_ERROR
        "No AOT output in ${DARK_GENERATED_DIR}.\n"
        "Generate it first: python tools/recompile.py --output ${DARK_GENERATED_DIR} --verify")
endif()
include("${DARK_GENERATED_DIR}/sources.cmake")

# The translated game, with the generated trapping bodies for the few imports
# the native runtime does not implement, exactly as the Windows build links it.
set(dark_ppc_sources ${DARK_PPC_SOURCES})

# The XMA decoder: the Windows build's pinned FFmpeg, cross-compiled for the
# console by tools/build-xma-codec.sh.
set(DARK_XMA_ROOT "${ROOT}/build_native/deps/ffmpeg-darkxma-ps5" CACHE PATH "the console's XMA decoder")
if(NOT EXISTS "${DARK_XMA_ROOT}/lib/libavcodec-darkxma.a")
    message(FATAL_ERROR "No console XMA decoder in ${DARK_XMA_ROOT}.\n"
                        "Build it first: app/ps5/tools/build-xma-codec.sh")
endif()

# The renderer's shaders. The console has no shader compiler and no module to
# load from: glslang compiles each stage to SPIR-V at build time (RADV compiles
# SPIR-V to machine code at run time, as it does for every title), and the
# modules are embedded as words the title links.
find_program(GLSLANG_VALIDATOR NAMES glslangValidator glslang)
if(NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR
        "glslangValidator is required: the PS5 renderer's shaders are compiled to SPIR-V at build time")
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(DARK_SHADER_SOURCE_DIR ${ROOT}/renderer/ps5/shaders)
set(DARK_SHADER_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${DARK_SHADER_BUILD_DIR})
set(DARK_SHADER_MODULES)
set(DARK_SHADER_ARGUMENTS)
foreach(shader IN ITEMS mesh.vert mesh_alpha.frag mesh_color.frag mesh_video.frag
                        transfer.vert transfer.frag present.frag fg_luma.comp fg_flow.comp fg_cut.comp fg_warp.comp)
    set(spirv ${DARK_SHADER_BUILD_DIR}/${shader}.spv)
    add_custom_command(
        OUTPUT ${spirv}
        COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.1
                -o ${spirv} ${DARK_SHADER_SOURCE_DIR}/${shader}
        DEPENDS ${DARK_SHADER_SOURCE_DIR}/${shader}
        COMMENT "glslang: ${shader}"
        VERBATIM)
    list(APPEND DARK_SHADER_MODULES ${spirv})
endforeach()
# The generated header names each module after the C++ array that holds it.
set(DARK_SHADER_ARGUMENTS
    "kMeshVertex=${DARK_SHADER_BUILD_DIR}/mesh.vert.spv"
    "kMeshAlpha=${DARK_SHADER_BUILD_DIR}/mesh_alpha.frag.spv"
    "kMeshColor=${DARK_SHADER_BUILD_DIR}/mesh_color.frag.spv"
    "kMeshVideo=${DARK_SHADER_BUILD_DIR}/mesh_video.frag.spv"
    # The engine's own resolve and frontbuffer copy. The world renderer's copies
    # are a pixel transfer, not geometry, and this is that transfer on RADV.
    "kWorldTransferVertex=${DARK_SHADER_BUILD_DIR}/transfer.vert.spv"
    "kWorldTransferFragment=${DARK_SHADER_BUILD_DIR}/transfer.frag.spv"
    # The frontbuffer copy that fills the panel, whatever its size.
    "kWorldPresentFragment=${DARK_SHADER_BUILD_DIR}/present.frag.spv"
    "kFgLuma=${DARK_SHADER_BUILD_DIR}/fg_luma.comp.spv"
    "kFgFlow=${DARK_SHADER_BUILD_DIR}/fg_flow.comp.spv"
    "kFgCut=${DARK_SHADER_BUILD_DIR}/fg_cut.comp.spv"
    "kFgWarp=${DARK_SHADER_BUILD_DIR}/fg_warp.comp.spv")
set(DARK_SHADER_HEADER ${DARK_SHADER_BUILD_DIR}/ps5_shaders.h)
add_custom_command(
    OUTPUT ${DARK_SHADER_HEADER}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/spirv-header.py
            ${DARK_SHADER_HEADER} ${DARK_SHADER_ARGUMENTS}
    DEPENDS ${DARK_SHADER_MODULES} ${CMAKE_CURRENT_SOURCE_DIR}/tools/spirv-header.py
    COMMENT "embedding SPIR-V"
    VERBATIM)

# The engine's material tables (world_mesh.cpp) are generated from the same
# assets the Windows build reads. They are data, not shaders: the capture layer
# needs them to classify a draw's texture stages before the renderer exists.
set(DARK_ENGINE_SHADER_DIR ${ROOT}/build_native/shaders)
set(DARK_FRAGMENT_BINDINGS ${DARK_ENGINE_SHADER_DIR}/engine_world_fragment_bindings.generated.h)
if(NOT EXISTS ${DARK_FRAGMENT_BINDINGS})
    message(STATUS "Generating the engine material tables in ${DARK_ENGINE_SHADER_DIR}")
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/compile_world_fragment.py
                --game-dir ${ROOT}/Darkness --output-dir ${DARK_ENGINE_SHADER_DIR}
        RESULT_VARIABLE fragment_result
        OUTPUT_QUIET)
    if(NOT fragment_result EQUAL 0 OR NOT EXISTS ${DARK_FRAGMENT_BINDINGS})
        message(FATAL_ERROR
            "Cannot generate the engine material tables from ${ROOT}/Darkness.\n"
            "The capture layer classifies draws with them; put your own dump there first.")
    endif()
endif()

# The world renderer's shaders. The Windows renderer compiles HLSL at runtime;
# the console cannot, so every authored world vertex program and every original
# fragment program is compiled to SPIR-V here, once (tools/world-shaders.py
# caches by a stamp, so an unchanged corpus costs a second).
set(DARK_WORLD_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/world-shaders)
message(STATUS "Building the world shader corpus in ${DARK_WORLD_SHADER_DIR}")
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/world-shaders.py
            --game-dir ${ROOT}/Darkness --output ${DARK_WORLD_SHADER_DIR}
    RESULT_VARIABLE world_shader_result)
if(NOT world_shader_result EQUAL 0 OR
   NOT EXISTS ${DARK_WORLD_SHADER_DIR}/world_shaders.generated.h)
    message(FATAL_ERROR
        "Cannot build the world shader corpus from ${ROOT}/Darkness.\n"
        "Put your own dump there and make sure glslangValidator and python3 are on the path.")
endif()

# The native runtime's menu strings, generated as the Windows build generates
# them.
set(DARK_NATIVE_ASSETS_DIR ${CMAKE_CURRENT_BINARY_DIR}/native_assets)
set(DARK_MENU_TEXT_HEADER ${DARK_NATIVE_ASSETS_DIR}/native_menu_text.generated.h)
add_custom_command(
    OUTPUT ${DARK_MENU_TEXT_HEADER}
    COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/compile_native_menu_text.py
            --catalog ${ROOT}/assets/localization/native_menu_ru.json --output ${DARK_MENU_TEXT_HEADER}
    DEPENDS ${ROOT}/tools/compile_native_menu_text.py ${ROOT}/assets/localization/native_menu_ru.json
            ${ROOT}/assets/localization/subtitle_menu.json
    COMMENT "native menu text"
    VERBATIM)

add_executable(darkness_ps5
    # The module's entry: the console calls it with the process parameters it
    # wrote into the module, which the SDK's crt1.o (a process startup) does not
    # take. See app_crt.cpp.
    app_crt.cpp
    title_main.cpp
    iso_import.cpp
    game_setup.cpp
    cpu_sampling.cpp
    # The console's libc answers two calls the way a title cannot live with;
    # posix_compat.cpp corrects them for the whole module.
    posix_compat.cpp
    # The console's libc compares memory a byte at a time; fast_memcmp.cpp
    # replaces memcmp and bcmp for the whole module (see the wraps below).
    fast_memcmp.cpp
    fast_memory.cpp
    video_preferences.cpp
    # The Windows build's Xbox 360 kernel layer (runtime/native), compiled
    # against the Win32 subset it uses (runtime/native/win32_ps5.*, reached
    # through the stand-in headers in win32/).
    ${ROOT}/runtime/native/win32_ps5.cpp
    ${ROOT}/runtime/native/runtime.cpp
    ${ROOT}/runtime/native/kernel.cpp
    ${ROOT}/runtime/native/dispatcher.cpp
    ${ROOT}/runtime/native/filesystem.cpp
    ${ROOT}/runtime/native/storage.cpp
    ${ROOT}/runtime/native/guest_runner.cpp
    ${ROOT}/runtime/native/input.cpp
    ${ROOT}/runtime/native/xma_bridge.cpp
    ${ROOT}/runtime/native/xma_raw_decoder.cpp
    ${ROOT}/runtime/native/xex_image.cpp
    ${ROOT}/runtime/native/native_menu_assets.cpp
    ${ROOT}/runtime/native/language_settings.cpp
    ${ROOT}/runtime/native/native_menu_text.cpp
    ${DARK_MENU_TEXT_HEADER}
    ${ROOT}/runtime/native/achievements.cpp
    ${ROOT}/runtime/native/achievements_catalog.cpp
    ${ROOT}/runtime/native/fov_settings.cpp
    ${ROOT}/runtime/native/fov_camera.cpp
    ${ROOT}/runtime/native/camera_interpolation.cpp
    ${ROOT}/runtime/native/graphics_settings.cpp
    ${ROOT}/runtime/native/video_settings_menu.cpp
    ${ROOT}/runtime/native/mouse_look.cpp
    ${ROOT}/runtime/native/menu_pointer.cpp
    ${ROOT}/runtime/native/keyboard_bindings.cpp
    ${ROOT}/runtime/native/keyboard_menu.cpp
    ${ROOT}/runtime/native/keyboard_menu_guest.cpp
    ${ROOT}/runtime/native/developer_tools.cpp
    ${ROOT}/runtime/native/developer_player.cpp
    ${ROOT}/runtime/native/developer_missions.cpp
    ${ROOT}/runtime/native/developer_invincibility.cpp
    ${ROOT}/runtime/native/developer_player_lookup.cpp
    ${ROOT}/runtime/native/developer_update.cpp
    ${ROOT}/runtime/native/developer_noclip.cpp
    ${ROOT}/runtime/native/developer_darkness.cpp
    ${ROOT}/runtime/native/developer_weapon_assets.cpp
    ${ROOT}/runtime/native/stall_profiler.cpp
    # The console's devices under that layer: the pad (behind XInput), the
    # audio output (behind the render driver) and the subtitle font fallback.
    ${ROOT}/runtime/native/input_ps5.cpp
    camera_timing.cpp
    ${ROOT}/runtime/native/audio_ps5.cpp
    ${ROOT}/runtime/native/audio_driver_ps5.cpp
    ${ROOT}/renderer/engine/subtitle_font_console.cpp
    # The display: RADV's VK_KHR_display surface on the console's video output,
    # and the renderer that draws the engine's frames into it.
    ${ROOT}/renderer/ps5/display.cpp
    ${ROOT}/renderer/ps5/graphics_backend.cpp
    ${ROOT}/renderer/ps5/frame_interpolation.cpp
    # The engine's own render targets: their clears, resolves and the frontbuffer
    # copy into the swapchain, and the world draws that render into them.
    ${ROOT}/renderer/ps5/world_surfaces.cpp
    ${ROOT}/renderer/ps5/world_shaders.cpp
    ${ROOT}/renderer/ps5/world_draw.cpp
    # The debug frame dump: the presented image copied out and written as PNG.
    ${ROOT}/renderer/ps5/png_writer.cpp
    # The capture layer: the guest's draw calls, as the engine's renderer sees
    # them (render_trace.cpp's hooks), turned into the command stream the
    # backend consumes (simple_mesh.cpp's decoders and frame queue).
    ${ROOT}/renderer/engine/render_trace.cpp
    ${ROOT}/renderer/engine/simple_mesh.cpp
    ${ROOT}/renderer/engine/stored_geometry.cpp
    ${ROOT}/renderer/engine/world_mesh.cpp
    ${ROOT}/renderer/engine/scene_work.cpp
    ${ROOT}/renderer/engine/engine_transforms.cpp
    ${ROOT}/renderer/engine/engine_texture_constants.cpp
    ${ROOT}/renderer/engine/engine_vertex_descriptor.cpp
    ${ROOT}/renderer/engine/engine_palette.cpp
    ${ROOT}/renderer/engine/engine_vertex_program.cpp
    ${DARK_SHADER_HEADER}
    # The translated game itself.
    ${dark_ppc_sources})

if(DARK_PS5_NATIVE_COMMAND_EMISSION)
    target_sources(darkness_ps5 PRIVATE command_emission.cpp)
endif()

target_include_directories(darkness_ps5 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    # The stand-ins for <windows.h> and friends come before the SDK's headers.
    ${CMAKE_CURRENT_SOURCE_DIR}/win32
    ${DARK_NATIVE_ASSETS_DIR}
    ${DARK_XMA_ROOT}/include
    ${ROOT}
    ${ROOT}/renderer/ps5
    ${DARK_SHADER_BUILD_DIR}
    ${DARK_WORLD_SHADER_DIR}
    ${DARK_ENGINE_SHADER_DIR}
    ${DARK_GENERATED_DIR}
    ${DARK_SIMDE_DIR}
    ${VULKAN_HEADERS})

# The layout is a link input that CMake does not otherwise track: an edit to
# it must relink the title, not leave the previous ELF in place.
set_target_properties(darkness_ps5 PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    LINK_DEPENDS "${DARK_PS5_LAYOUT}")

# -fno-omit-frame-pointer: a console crash's backtrace is a frame-pointer walk.
# SSSE3 is baseline on the console's CPU, and the AOT vector helpers compile
# against it on the Windows build too.
target_compile_options(darkness_ps5 PRIVATE
    -O2 -g1 -mssse3 -ffunction-sections -fdata-sections -fno-omit-frame-pointer)
target_compile_definitions(darkness_ps5 PRIVATE DARK_NATIVE_SSSE3=1)

if(DARK_PS5_THIN_LTO)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "The PS5 ThinLTO experiment requires Clang and LLD")
    endif()
    target_compile_options(darkness_ps5 PRIVATE -flto=thin)
    target_link_options(darkness_ps5 PRIVATE -flto=thin
        "-Wl,--thinlto-cache-dir=${CMAKE_CURRENT_BINARY_DIR}/thinlto-cache"
        -Wl,--thinlto-jobs=6)
    set_source_files_properties(${dark_ppc_sources} PROPERTIES
        COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/lto_guest_calls.h")
    # The local WSL VM has 8 GiB. Keep translation and link-backend memory
    # bounded independently of the host's advertised processor count.
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS dark_lto_compile=4)
    set_property(TARGET darkness_ps5 PROPERTY JOB_POOL_COMPILE dark_lto_compile)
endif()
target_link_options(darkness_ps5 PRIVATE
    # A title imports libkernel, not the web process's libkernel_web. The SDK's
    # driver adds -lkernel_web itself (its libkernel_stub_weak.so is named
    # libkernel_web.sprx), and the console refused the module that imported it:
    # the title was launched and logged 0xa0020102 (PRX_SCE_MODULE_LOAD_ERROR)
    # before a single instruction ran. Naming libkernel drops the driver's own
    # choice, and --as-needed keeps its weak stub - which resolves nothing once
    # libkernel is there - out of the import table.
    -lkernel
    -Wl,--as-needed
    # No crt1.o: the entry is app_crt.cpp's _start, which takes the console's
    # process parameters. crtbegin/crti/crtend go with it; the constructors and
    # destructors they would register are run by _init/_fini instead.
    -nostartfiles
    # The C11 threads layer in RADV's archive answers with the console's
    # <threads.h> codes and Mesa's callers read Mesa's: posix_compat.cpp
    # translates them at the calls that return one.
    -Wl,--wrap=mtx_init
    -Wl,--wrap=mtx_lock
    -Wl,--wrap=mtx_timedlock
    -Wl,--wrap=mtx_trylock
    -Wl,--wrap=mtx_unlock
    -Wl,--wrap=cnd_init
    -Wl,--wrap=cnd_wait
    -Wl,--wrap=cnd_timedwait
    -Wl,--wrap=cnd_signal
    -Wl,--wrap=cnd_broadcast
    -Wl,--wrap=thrd_create
    -Wl,--wrap=thrd_join
    -Wl,--wrap=thrd_detach
    # The sandbox refuses realpath for the title's own folder; posix_compat.cpp
    # resolves it lexically then.
    -Wl,--wrap=realpath
    # Nothing reads a title's stdout; posix_compat.cpp sends it to stderr.
    -Wl,--wrap=printf
    -Wl,--wrap=vprintf
    -Wl,--wrap=puts
    -Wl,--wrap=putchar
    # The console's byte-at-a-time memcmp was the title's hottest instruction;
    # fast_memcmp.cpp compares 16 bytes at a time with the same results.
    -Wl,--wrap=memcmp
    -Wl,--wrap=bcmp
    -Wl,--wrap=memcpy
    -Wl,--wrap=memmove
    -Wl,--wrap=memset
    -Wl,--exclude-libs=ALL
    -Wl,--eh-frame-hdr
    # No build-id note: it would land inside the data segment, ahead of .data,
    # and the native tool places the PS5 process parameters after the last
    # read-only section, where they would then collide with the data.
    -Wl,--build-id=none
    "-Wl,-T,${DARK_PS5_LAYOUT}")

# The console modules the ported backends call. SceSystemService is the Shell
# call that takes the launch splash down once the game has a frame of its own
# (title_main.cpp); a title that never makes it stays behind the splash.
target_link_libraries(darkness_ps5 PRIVATE ScePad SceAudioOut SceUserService SceSystemService
    ${DARK_XMA_ROOT}/lib/libavcodec-darkxma.a ${DARK_XMA_ROOT}/lib/libavutil-darkxma.a)
target_link_options(darkness_ps5 PRIVATE -Wl,--wrap=sceVideoOutOpen)

# The console's Vulkan: RADV, linked into the title, presenting through the
# video output's WSI (renderer/ps5/display.cpp).
include(${CMAKE_CURRENT_SOURCE_DIR}/radv.cmake)

# The title reads the player's own dump from its game directory, so the build
# output is the executable and its metadata. tools/build.sh stages it.
