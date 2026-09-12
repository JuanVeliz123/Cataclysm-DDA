add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third-party)

set(MAIN_CPP ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)
set(MESSAGES_CPP ${CMAKE_CURRENT_SOURCE_DIR}/messages.cpp)
set(RESOURCE_RC ${CMAKE_CURRENT_SOURCE_DIR}/resource.rc)

if (WIN32)
    set(RSRC_RC_DEP "${CMAKE_SOURCE_DIR}/data/cataicon.ico")
    set(RSRC_RC_DEP "${RSRC_RC_DEP};${CMAKE_SOURCE_DIR}/data/application_manifest.xml")
    set_source_files_properties(
            ${RESOURCE_RC}
            PROPERTIES
            OBJECT_DEPENDS "${RSRC_RC_DEP}")
endif ()

file(GLOB CATACLYSM_DDA_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/cata_allocator_c.c)

if (MSVC)
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/mapgen.cpp
        PROPERTIES COMPILE_OPTIONS "/bigobj"
    )
endif()

if (NOT WARN_STALE_DATA)
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/flexbuffer_cache.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/options.cpp
        PROPERTIES COMPILE_DEFINITIONS NO_STALE_DATA_WARN
    )
endif()

list(REMOVE_ITEM CATACLYSM_DDA_SOURCES ${MAIN_CPP} ${MESSAGES_CPP})

file(GLOB CATACLYSM_DDA_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/*.h)

# Get GIT version strings
add_custom_target(
        get_version
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/version.h
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_custom_command(
        OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/version.h
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/version.cmake
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Build tiles version if requested
# Build Godot extension if requested
if (GODOT)
    # godot-cpp ships its own CMake project which exports the `godot-cpp` target
    # with the right per-platform definitions (LINUX_ENABLED / MACOS_ENABLED /
    # WINDOWS_ENABLED, DEBUG_ENABLED, HOT_RELOAD_ENABLED) and PIC already set.
    # Building it as a subproject keeps the extension's ABI in lockstep with the
    # bindings, which a hand-rolled IMPORTED target cannot guarantee.
    if (NOT TARGET godot-cpp)
        if (NOT GODOT_CPP_DIR)
            if (DEFINED ENV{GODOT_CPP_DIR})
                set(GODOT_CPP_DIR "$ENV{GODOT_CPP_DIR}")
            else ()
                set(GODOT_CPP_DIR "$ENV{HOME}/godot-cpp")
            endif ()
        endif ()
        set(GODOT_CPP_DIR "${GODOT_CPP_DIR}" CACHE PATH "Path to a godot-cpp checkout")
        if (NOT EXISTS "${GODOT_CPP_DIR}/CMakeLists.txt")
            message(FATAL_ERROR
                    "godot-cpp not found at ${GODOT_CPP_DIR}. Clone it with "
                    "build-scripts/get-godot-cpp.sh or pass -DGODOT_CPP_DIR=<path>.")
        endif ()
        add_subdirectory("${GODOT_CPP_DIR}" "${CMAKE_BINARY_DIR}/godot-cpp" EXCLUDE_FROM_ALL)
    endif ()

    # Create a shared library for the extension
    # Exclude SDL files if building for Godot. cata_imgui.cpp is kept and
    # guards its SDL includes behind !defined(GODOT).
    foreach(SDL_FILE sdltiles.cpp sdl_font.cpp sdl_geometry.cpp sdl_wrappers.cpp sdl_utils.cpp sdl_gamepad.cpp sdlsound.cpp sound_backend_sdl2.cpp sound_backend_sdl3.cpp pixel_minimap.cpp tileset_loader.cpp)
        list(REMOVE_ITEM CATACLYSM_DDA_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/${SDL_FILE})
    endforeach()

    # messages.cpp is normally built into the game executable; the Godot
    # extension needs it for the add_msg* family (messages.h/messages.cpp).
    list(APPEND CATACLYSM_DDA_SOURCES ${MESSAGES_CPP})

    add_library(cataclysm-godot SHARED
            ${CATACLYSM_DDA_SOURCES}
            ${CATACLYSM_DDA_HEADERS}
            ${CMAKE_SOURCE_DIR}/godot/extensions/godot_ext.cpp)

    target_include_directories(cataclysm-godot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/godot/extensions)
    # godot-cpp propagates its platform/feature definitions and include dirs
    # through this target, so nothing here needs to restate them.
    target_link_libraries(cataclysm-godot PRIVATE third-party imgui godot-cpp ZLIB::ZLIB)

    # The extension is a shared library; its static dependencies must be PIC.
    set_property(TARGET cataclysm-godot third-party imgui
            PROPERTY POSITION_INDEPENDENT_CODE ON)

    if(APPLE)
        target_link_libraries(cataclysm-godot PRIVATE "-framework CoreFoundation" "-framework SystemConfiguration")
    endif()

    target_compile_definitions(cataclysm-godot PUBLIC GODOT)

    # Name the artifact exactly as godot/extensions/cataclysm.gdextension expects
    # per platform, so the Godot host can load it straight out of godot/bin/.
    if(APPLE)
        set(GODOT_EXT_FILENAME "libcataclysm-godot.dylib")
    else()
        get_target_property(GODOT_EXT_PLATFORM godot-cpp GODOTCPP_PLATFORM)
        get_target_property(GODOT_EXT_ARCH godot-cpp GODOTCPP_ARCH)
        get_target_property(GODOT_EXT_CPP_TARGET godot-cpp GODOTCPP_TARGET)
        if(GODOT_EXT_CPP_TARGET STREQUAL "template_release")
            set(GODOT_EXT_KIND "release")
        else()
            set(GODOT_EXT_KIND "debug")
        endif()
        if(WIN32)
            set(GODOT_EXT_SUFFIX ".dll")
        else()
            set(GODOT_EXT_SUFFIX ".so")
        endif()
        set(GODOT_EXT_FILENAME
                "libcataclysm-godot.${GODOT_EXT_PLATFORM}.${GODOT_EXT_KIND}.${GODOT_EXT_ARCH}${GODOT_EXT_SUFFIX}")
    endif()

    set_target_properties(cataclysm-godot PROPERTIES
            PREFIX ""
            SUFFIX ""
            OUTPUT_NAME "${GODOT_EXT_FILENAME}")

    # Post-build: copy to godot/bin/
    add_custom_command(TARGET cataclysm-godot POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/godot/bin/
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:cataclysm-godot> ${CMAKE_SOURCE_DIR}/godot/bin/
            COMMENT "Copying extension to Godot project"
    )
endif()

if (TILES)
    add_library(
            cataclysm-tiles-common OBJECT
            ${CATACLYSM_DDA_SOURCES}
            ${CATACLYSM_DDA_HEADERS})
    target_include_directories(cataclysm-tiles-common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

    target_link_libraries(cataclysm-tiles-common PUBLIC third-party)

    if (WIN32)
        add_definitions(-DUSE_WINMAIN)
        add_executable(
                cataclysm-tiles WIN32
                ${MAIN_CPP}
                ${MESSAGES_CPP}
                ${RESOURCE_RC})
        if(MSYS2)
            set(_KEEP $<$<CONFIG:RelWithDebInfo,Debug>:objcopy
            --only-keep-debug
            $<TARGET_FILE:cataclysm-tiles>
            $<TARGET_FILE:cataclysm-tiles>.debug
            >)
            set(_STRIP $<$<CONFIG:RelWithDebInfo,Debug>:objcopy
            --strip-unneeded
            --add-gnu-debuglink=$<TARGET_FILE:cataclysm-tiles>.debug
            $<TARGET_FILE:cataclysm-tiles>
            >)
            add_custom_command(TARGET cataclysm-tiles POST_BUILD
                COMMAND "${_KEEP}"
                COMMAND "${_STRIP}"
                COMMAND_EXPAND_LISTS
            )
        else()
            # Use only our RT_MANIFEST from resource.rc
            target_link_options(cataclysm-tiles PRIVATE
                "LINKER:/MANIFEST:NO"
            )
        endif()
    else ()
        add_executable(cataclysm-tiles
                ${MAIN_CPP}
                ${MESSAGES_CPP})
    endif ()

    add_dependencies(cataclysm-tiles-common get_version)

    target_link_libraries(cataclysm-tiles PRIVATE cataclysm-tiles-common)
    target_compile_definitions(cataclysm-tiles-common PUBLIC TILES )
    if(NOT "${CMAKE_EXPORT_COMPILE_COMMANDS}")
        target_precompile_headers(cataclysm-tiles-common PUBLIC
            "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_SOURCE_DIR}/pch/main-pch.hpp>")
    endif ()
    if (LOCALIZE)
        target_include_directories(cataclysm-tiles-common PUBLIC
                ${LIBINTL_INCLUDE_DIR}
                ${ICONV_INCLUDE_DIR})
        target_link_libraries(cataclysm-tiles-common PUBLIC
                ${LIBINTL_LIBRARIES}
                ${ICONV_LIBRARIES})
    endif ()

    if (CMAKE_USE_PTHREADS_INIT)
        target_compile_options(cataclysm-tiles-common PUBLIC "-pthread")
    endif ()

    if (CMAKE_THREAD_LIBS_INIT)
        target_link_libraries(cataclysm-tiles-common PUBLIC ${CMAKE_THREAD_LIBS_INIT})
    endif ()

    if (NOT DYNAMIC_LINKING)
        if (TARGET SDL3::SDL3-static)
            set(_SDL3 SDL3::SDL3-static)
        else ()
            set(_SDL3 SDL3::SDL3)
        endif ()
        if (TARGET SDL3_image::SDL3_image-static)
            set(_SDL3_IMAGE SDL3_image::SDL3_image-static)
        else ()
            set(_SDL3_IMAGE SDL3_image::SDL3_image)
        endif ()
        if (TARGET SDL3_ttf::SDL3_ttf-static)
            set(_SDL3_TTF SDL3_ttf::SDL3_ttf-static)
        else ()
            set(_SDL3_TTF SDL3_ttf::SDL3_ttf)
        endif ()
        target_link_libraries(cataclysm-tiles-common PUBLIC
            ${_SDL3}
            ${_SDL3_IMAGE}
            ${_SDL3_TTF}
        )
    else()
        target_link_libraries(cataclysm-tiles-common PUBLIC
            SDL3::SDL3
            SDL3_image::SDL3_image
            SDL3_ttf::SDL3_ttf
        )
    endif ()
    if (SOUND)
        if (NOT DYNAMIC_LINKING AND TARGET SDL3_mixer::SDL3_mixer-static)
            target_link_libraries(cataclysm-tiles-common PUBLIC
                SDL3_mixer::SDL3_mixer-static
            )
        else()
            target_link_libraries(cataclysm-tiles-common PUBLIC
                SDL3_mixer::SDL3_mixer
            )
        endif()
        add_definitions(-DSDL_SOUND)
    endif ()

    if (WIN32)
        # Global settings for Windows targets (at end)
        target_link_libraries(cataclysm-tiles-common PUBLIC gdi32.lib)
        target_link_libraries(cataclysm-tiles-common PUBLIC winmm.lib)
        target_link_libraries(cataclysm-tiles-common PUBLIC imm32.lib)
        target_link_libraries(cataclysm-tiles-common PUBLIC ole32.lib)
        target_link_libraries(cataclysm-tiles-common PUBLIC oleaut32.lib)
        target_link_libraries(cataclysm-tiles-common PUBLIC version.lib)
        target_link_libraries(cataclysm-tiles-common PUBLIC setupapi.lib)
        if (SOUND)
            target_link_libraries(cataclysm-tiles-common PUBLIC shlwapi.lib)
            target_link_libraries(cataclysm-tiles-common PUBLIC dwrite.lib)
        endif()
        if (BACKTRACE)
            target_link_libraries(cataclysm-tiles-common PUBLIC dbghelp.lib)
            if (LIBBACKTRACE)
                target_link_libraries(cataclysm-tiles-common PUBLIC backtrace)
            endif ()
        endif ()
    elseif (APPLE)
        target_link_libraries(cataclysm-tiles-common PUBLIC "-framework CoreFoundation")
    endif ()

    if (LIBBACKTRACE)
        target_link_libraries(cataclysm-tiles-common PUBLIC backtrace)
    endif ()

    if (RELEASE)
        install(TARGETS cataclysm-tiles RUNTIME)
    endif ()

    if (TARGET ZLIB::ZLIB)
        target_link_libraries(cataclysm-tiles-common PUBLIC ZLIB::ZLIB)
    else()
        target_link_libraries(cataclysm-tiles-common PUBLIC ${ZLIB_LIBRARIES})
    endif()

    # Compile GLSL shaders to per-backend artifacts (.spv / .dxil / .msl)
    # for the GPU shader path. Pattern mirrors lang/CMakeLists.txt locale
    # build. Default the format set to the local GPU backend so that
    # builds without shadercross succeed (variant_pass goes inert at
    # runtime if the expected artifact format is missing). Override
    # CATA_SHADER_FORMATS when shipping a multi-platform bindist.
    set(_SHADERS_DIR ${CMAKE_SOURCE_DIR}/data/shaders)
    if (NOT DEFINED CATA_SHADER_FORMATS)
        if (APPLE)
            set(CATA_SHADER_FORMATS "msl")
        elseif (WIN32)
            set(CATA_SHADER_FORMATS "dxil")
        else()
            set(CATA_SHADER_FORMATS "spv")
        endif()
    endif()
    # Stamp filename encodes the format set so switching CATA_SHADER_FORMATS
    # invalidates the stamp and triggers a rebuild. Hyphen-separated to
    # avoid commas in filenames; matches the Makefile and MSVC prebuild.
    string(REPLACE "," "-" _SHADER_STAMP_TAG "${CATA_SHADER_FORMATS}")
    set(_SHADERS_STAMP ${_SHADERS_DIR}/build-${_SHADER_STAMP_TAG}.stamp)
    file(GLOB _SHADER_SOURCES "${_SHADERS_DIR}/*.frag" "${_SHADERS_DIR}/*.vert")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_custom_command(
            OUTPUT ${_SHADERS_STAMP}
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tools/build_shaders.py
                --shader-dir ${_SHADERS_DIR}
                --formats ${CATA_SHADER_FORMATS}
                --stamp ${_SHADERS_STAMP}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            DEPENDS ${_SHADER_SOURCES}
                    ${CMAKE_SOURCE_DIR}/tools/build_shaders.py
            COMMENT "Compiling shader artifacts (${CATA_SHADER_FORMATS})"
    )
    add_custom_target(compile_shaders ALL DEPENDS ${_SHADERS_STAMP})
    add_dependencies(cataclysm-tiles-common compile_shaders)

endif ()

# Build curses version if requested
if (CURSES)
    add_library(cataclysm-common OBJECT
            ${CATACLYSM_DDA_SOURCES}
            ${CATACLYSM_DDA_HEADERS})
    target_include_directories(cataclysm-common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cataclysm-common PUBLIC third-party)

    if (WIN32)
        add_executable(cataclysm
                ${MAIN_CPP}
                ${MESSAGES_CPP}
                ${RESOURCE_RC})
        if(NOT MINGW)
            # Use only our RT_MANIFEST from resource.rc
            target_link_options(cataclysm PRIVATE
                "LINKER:/MANIFEST:NO"
            )
        endif()
    else ()
        add_executable(cataclysm
                ${MAIN_CPP}
                ${MESSAGES_CPP})
    endif ()

    add_dependencies(cataclysm-common get_version)
    target_link_libraries(cataclysm PRIVATE cataclysm-common)

    if (LOCALIZE)
        target_include_directories(cataclysm-common PUBLIC
                ${LIBINTL_INCLUDE_DIR}
                ${ICONV_INCLUDE_DIR})
        target_link_libraries(cataclysm-common PUBLIC
                ${LIBINTL_LIBRARIES}
                ${ICONV_LIBRARIES})
    endif ()

    target_include_directories(cataclysm-common PUBLIC
        ${CURSES_INCLUDE_DIR} ${NCURSES_INCLUDE_DIRS}
    )
    target_link_libraries(cataclysm-common PUBLIC ${CURSES_LIBRARIES})

    if (CMAKE_USE_PTHREADS_INIT)
        target_compile_options(cataclysm-common PUBLIC "-pthread")
    endif ()

    if (CMAKE_THREAD_LIBS_INIT)
        target_link_libraries(cataclysm-common PUBLIC ${CMAKE_THREAD_LIBS_INIT})
    endif ()

    if (WIN32)
        # Global settings for Windows targets (at end)
        if(USE_PDCURSES)
            target_compile_definitions(cataclysm-common PUBLIC USE_PDCURSES)
        endif()
        target_link_libraries(cataclysm-common PUBLIC gdi32.lib)
        target_link_libraries(cataclysm-common PUBLIC winmm.lib)
        target_link_libraries(cataclysm-common PUBLIC imm32.lib)
        target_link_libraries(cataclysm-common PUBLIC ole32.lib)
        target_link_libraries(cataclysm-common PUBLIC oleaut32.lib)
        target_link_libraries(cataclysm-common PUBLIC version.lib)
        if (BACKTRACE)
            target_link_libraries(cataclysm-common PUBLIC dbghelp.lib)
        endif ()
    elseif (APPLE)
        target_link_libraries(cataclysm-common PUBLIC "-framework CoreFoundation")
    endif ()

    if (LIBBACKTRACE)
        target_link_libraries(cataclysm-common PUBLIC backtrace)
    endif ()

    if (RELEASE)
        install(TARGETS cataclysm RUNTIME)
    endif ()

    if (TARGET ZLIB::ZLIB)
        target_link_libraries(cataclysm-common PUBLIC ZLIB::ZLIB)
    else()
        target_link_libraries(cataclysm-common PUBLIC ${ZLIB_LIBRARIES})
    endif()
endif ()

if (MINGW AND NOT RELEASE)
    # Try to Install shared libraries for dev builds
    # Note: It is specific to MSYS2 and uses hardcoded versions so
    # probably it will fail if you run it :)

    # GCC-specific libraries and zlib
    find_library(RuntimeLib_GCC_S_DW2_1  "gcc_s_dw2-1")
    find_library(RuntimeLib_STDC_PP_6    "stdc++-6")
    find_library(RuntimeLib_WINPTHREAD_1 "winpthread-1")
    find_library(RuntimeLib_zlib  "zlib1")

    set(RuntimeLib_GCC_ALL
            ${RuntimeLib_GCC_S_DW2_1}
            ${RuntimeLib_STDC_PP_6}
            ${RuntimeLib_WINPTHREAD_1}
            ${RuntimeLib_zlib})
    if (TILES)
        # SDL can have a varius deps. Here you are the MSYS2 ones...
        find_library(RuntimeLib_SDL3  "SDL3")
        find_library(RuntimeLib_SDL3_IMG  "SDL3_image")
        find_library(RuntimeLib_png  "libpng16-16")
        find_library(RuntimeLib_jpeg  "libjpeg-8")
        find_library(RuntimeLib_jbig  "libjbig-0")
        find_library(RuntimeLib_tiff  "libtiff-5")
        find_library(RuntimeLib_webp  "libwebp-5")
        find_library(RuntimeLib_lzma  "liblzma-5")
        find_library(RuntimeLib_bz2  "libbz2-1")
        find_library(RuntimeLib_hb  "libharfbuzz-0")
        find_library(RuntimeLib_SDL3_TTF  "SDL3_ttf")
        find_library(RuntimeLib_ft  "libfreetype-6")
        find_library(RuntimeLib_glib  "libglib-2.0-0")
        set(RuntimeLib_SDL
                ${RuntimeLib_SDL3}
                ${RuntimeLib_SDL3_IMG}
                ${RuntimeLib_png}
                ${RuntimeLib_jpeg}
                ${RuntimeLib_jbig}
                ${RuntimeLib_tiff}
                ${RuntimeLib_webp}
                ${RuntimeLib_lzma}
                ${RuntimeLib_bz2}
                ${RuntimeLib_hb}
                ${RuntimeLib_SDL3_TTF}
                ${RuntimeLib_ft}
                ${RuntimeLib_glib})
        if (SOUND)
            find_library(RuntimeLib_SDL_SND  "SDL3_mixer")
            find_library(RuntimeLib_flac  "libFLAC-8")
            find_library(RuntimeLib_ogg  "libogg-0")
            find_library(RuntimeLib_flu  "libfluidsynth-1")
            find_library(RuntimeLib_port  "libportaudio-2")
            find_library(RuntimeLib_snd  "libsndfile-1")
            find_library(RuntimeLib_vorb  "libvorbis-0")
            find_library(RuntimeLib_vorb_enc  "libvorbisenc-2")
            find_library(RuntimeLib_vorb_f  "libvorbisfile-3")
            find_library(RuntimeLib_mod  "libmodplug-1")
            find_library(RuntimeLib_mpeg  "smpeg2")
            set(RuntimeLib_SDL_SOUND
                    ${RuntimeLib_SDL_SND}
                    ${RuntimeLib_flac}
                    ${RuntimeLib_ogg}
                    ${RuntimeLib_flu}
                    ${RuntimeLib_port}
                    ${RuntimeLib_snd}
                    ${RuntimeLib_vorb}
                    ${RuntimeLib_vorb_enc}
                    ${RuntimeLib_vorb_f}
                    ${RuntimeLib_mod}
                    ${RuntimeLib_mpeg})
        endif ()
    endif ()
    install(FILES ${RuntimeLib_GCC_ALL}
            ${RuntimeLib_LOCALIZE}
            ${RuntimeLib_SDL}
            ${RuntimeLib_SDL_SOUND}
            TYPE BIN)
endif ()
