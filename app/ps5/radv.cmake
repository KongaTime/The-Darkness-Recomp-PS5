# Linking RADV into the PS5 title.
#
# The console has no system Vulkan: RADV is linked into the title from
# PS5_Vulkan's release archive, which speaks to the video output through
# PS5_Mesa's VideoOut WSI. RADV needs three things from the link that a plain
# C++ link does not give it (PS5_Vulkan/tools/radv-link.sh is the same recipe):
#
#   * the platform layer (libps5platform.a) for the libc functions no system
#     module exports, bound to libc's names with --defsym,
#   * the platform's heap and thread entry points, reached with --wrap, because
#     libc's private heap and main-stack threads do not carry a title,
#   * every bound name kept local, or the title would export libc's names and
#     the module converter refuses the result.
#
# The defsym list is only applied for names the SDK's platform layer actually
# defines, so a title pinned to a different SDK still links.

set(RADV_ARCHIVE "${ROOT}/../PS5_Vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
    CACHE FILEPATH "RADV's release archive for the console (whole-archive)")
set(RADV_PLATFORM_LIBRARY "${CMAKE_SYSROOT}/lib/libps5platform.a"
    CACHE FILEPATH "the SDK's platform layer")
# RADV imports AGC (sceAgcDriver*), which the public SDK has no stubs for. These
# are minimal modules that name the imports, built by tools/build.sh.
set(DARK_AGC_STUBS "${CMAKE_BINARY_DIR}/stubs" CACHE PATH "AGC link stubs built before this link")

if(NOT EXISTS "${RADV_ARCHIVE}")
    message(FATAL_ERROR "No RADV archive at ${RADV_ARCHIVE}\nSet RADV_ARCHIVE to PS5_Vulkan's libvulkan_radeon.ps5.a")
endif()
if(NOT EXISTS "${RADV_PLATFORM_LIBRARY}")
    message(FATAL_ERROR "No platform layer at ${RADV_PLATFORM_LIBRARY}")
endif()

# Clang's builtins: RADV's emulated TLS calls __emutls_get_address, which is
# there rather than in the SDK's libraries.
execute_process(COMMAND "${CMAKE_CXX_COMPILER}" --print-resource-dir
    OUTPUT_VARIABLE DARK_CLANG_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
set(RADV_BUILTINS "${DARK_CLANG_RESOURCE_DIR}/lib/linux/libclang_rt.builtins-x86_64.a")
if(NOT EXISTS "${RADV_BUILTINS}" AND EXISTS "${CMAKE_SYSROOT}/lib/libclang_rt.builtins-x86_64.a")
    set(RADV_BUILTINS "${CMAKE_SYSROOT}/lib/libclang_rt.builtins-x86_64.a")
endif()
if(NOT EXISTS "${RADV_BUILTINS}")
    message(FATAL_ERROR "No clang builtins archive for RADV (tried ${RADV_BUILTINS})")
endif()

set(DARK_RADV_FLAGS)
set(DARK_RADV_BOUND_NAMES)

# What the platform layer exports, read once (llvm-nm's output is small here).
execute_process(COMMAND "${CMAKE_SYSROOT}/../bin/llvm-nm" --defined-only "${RADV_PLATFORM_LIBRARY}"
    OUTPUT_VARIABLE DARK_PLATFORM_SYMBOLS ERROR_QUIET)
if(NOT DARK_PLATFORM_SYMBOLS)
    execute_process(COMMAND llvm-nm --defined-only "${RADV_PLATFORM_LIBRARY}"
        OUTPUT_VARIABLE DARK_PLATFORM_SYMBOLS ERROR_QUIET)
endif()
string(REGEX MATCHALL " T ps5_[A-Za-z0-9_]+" DARK_PLATFORM_EXPORTS "${DARK_PLATFORM_SYMBOLS}")
list(TRANSFORM DARK_PLATFORM_EXPORTS REPLACE " T ps5_" "")

function(dark_bind name)
    if(name IN_LIST DARK_PLATFORM_EXPORTS)
        list(APPEND DARK_RADV_FLAGS "-Wl,--defsym=${name}=ps5_${name}")
        list(APPEND DARK_RADV_BOUND_NAMES "${name}")
        set(DARK_RADV_FLAGS "${DARK_RADV_FLAGS}" PARENT_SCOPE)
        set(DARK_RADV_BOUND_NAMES "${DARK_RADV_BOUND_NAMES}" PARENT_SCOPE)
    endif()
endfunction()

# Threads that ask for no stack get the platform's; the wraps also let RADV's
# and libc++'s joins free them.
foreach(name pthread_create pthread_join pthread_detach)
    list(APPEND DARK_RADV_FLAGS "-Wl,--wrap=${name}")
endforeach()

# Every allocation goes to the platform's heap in direct memory.
foreach(name malloc calloc realloc free posix_memalign aligned_alloc memalign
        malloc_usable_size reallocf reallocarray getline getdelim)
    list(APPEND DARK_RADV_FLAGS "-Wl,--wrap=${name}")
endforeach()
# The platform's stdio buffer publishing, and its FILE handling.
foreach(name fclose fflush)
    list(APPEND DARK_RADV_FLAGS "-Wl,--wrap=${name}")
endforeach()

# Names the platform layer stands in for. The unconditional ones exist in
# every supported SDK; the rest are checked against what it exports.
foreach(name qsort_r mkstemps openlog popen pclose __xuname __assert __memset_chk
        regcomp regexec regfree regerror localtime_r newlocale freelocale strtod_l
        strtof_l dladdr utimensat localeconv_l strtoll_l strtoull_l strtold_l snprintf_l
        sscanf_l asprintf_l strcoll_l strxfrm_l strftime_l wcscoll_l wcsxfrm_l btowc_l
        wctob_l iswctype_l mbrlen_l mbrtowc_l mbsrtowcs_l mbsnrtowcs_l wcrtomb_l
        wcsnrtombs_l mbtowc_l ___mb_cur_max_l ___runetype_l ___tolower_l ___toupper_l
        __runes_for_locale catopen catgets catclose backtrace backtrace_symbols_fd
        __cxa_thread_atexit_impl arc4random arc4random_buf arc4random_uniform gmtime_r
        statvfs fstatvfs futimens clock_nanosleep getaddrinfo freeaddrinfo if_nameindex
        if_freenameindex opendir fdopendir readdir rewinddir dirfd closedir nl_langinfo
        nl_langinfo_l getpwuid_r posix_fallocate access openat unlinkat fchmodat fstatat
        mkdirat renameat memfd_create localeconv strcasestr mkstemp readlink link symlink
        open_memstream)
    dark_bind(${name})
endforeach()

# A bound name stays local: exported, it would override the stub modules'.
set(DARK_RADV_VERSION_SCRIPT "${CMAKE_BINARY_DIR}/radv-platform-local.map")
file(WRITE "${DARK_RADV_VERSION_SCRIPT}.tmp" "{\n    local:\n")
foreach(name ${DARK_RADV_BOUND_NAMES})
    file(APPEND "${DARK_RADV_VERSION_SCRIPT}.tmp" "        ${name};\n")
endforeach()
file(APPEND "${DARK_RADV_VERSION_SCRIPT}.tmp" "};\n")
file(RENAME "${DARK_RADV_VERSION_SCRIPT}.tmp" "${DARK_RADV_VERSION_SCRIPT}")

message(STATUS "RADV: ${DARK_RADV_BOUND_NAMES} bound to the platform layer")

target_link_libraries(darkness_ps5 PRIVATE SceVideoOut)
foreach(agc_module libSceAgc libSceAgcDriver)
    if(NOT EXISTS "${DARK_AGC_STUBS}/${agc_module}.so")
        message(FATAL_ERROR
            "No ${agc_module}.so in ${DARK_AGC_STUBS}: RADV imports AGC.\n"
            "Build the title through tools/build.sh, which creates them.")
    endif()
    target_link_libraries(darkness_ps5 PRIVATE "${DARK_AGC_STUBS}/${agc_module}.so")
endforeach()

target_link_options(darkness_ps5 PRIVATE
    "-Wl,--whole-archive" "${RADV_ARCHIVE}" "-Wl,--no-whole-archive"
    # Only what the driver does not already add: it links libc++, libc++abi,
    # libunwind and libc itself, and naming them again collides on libc++'s C
    # threading entry points. The group keeps the platform layer's references
    # to RADV's and Clang's objects ordered.
    "-Wl,--start-group" "${RADV_BUILTINS}" "${RADV_PLATFORM_LIBRARY}" "-Wl,--end-group"
    ${DARK_RADV_FLAGS}
    "-Wl,--allow-multiple-definition"
    "-Wl,--version-script=${DARK_RADV_VERSION_SCRIPT}")
