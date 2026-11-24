#
# PowerMac Emulator - a Power Macintosh 8500/9500 emulator
# based on the DingusPPC PowerPC Macintosh emulation engine.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
cmake_minimum_required(VERSION 3.16)
project(PowerMacEmulator CXX C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(PlatformGlob)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ----------------------------------------------------------------------------
# SDL2
# ----------------------------------------------------------------------------
if (WIN32)
    # Look for a locally vendored SDL2 development package first so that a
    # fresh checkout builds without vcpkg or any system-wide SDL2 install.
    if (NOT DEFINED SDL2_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/thirdparty/SDL2/cmake/sdl2-config.cmake")
        set(SDL2_DIR "${CMAKE_SOURCE_DIR}/thirdparty/SDL2/cmake" CACHE PATH "SDL2 CMake package directory")
    endif()
    find_package(SDL2 CONFIG REQUIRED)
    add_compile_definitions(SDL_MAIN_HANDLED)
else ()
    find_package(SDL2 REQUIRED)
    include_directories(${SDL2_INCLUDE_DIRS})
    if (UNIX AND NOT APPLE)
        find_package(Threads REQUIRED)
    endif()
endif()

# ----------------------------------------------------------------------------
# Qt6 (VM manager and, when available, the emulator's native window)
# ----------------------------------------------------------------------------
if (NOT CMAKE_PREFIX_PATH AND DEFINED ENV{QTDIR})
    list(APPEND CMAKE_PREFIX_PATH "$ENV{QTDIR}")
endif()
if (NOT CMAKE_PREFIX_PATH AND EXISTS "I:/Qt/6.12.0/msvc2022_64/lib/cmake/Qt6")
    list(APPEND CMAKE_PREFIX_PATH "I:/Qt/6.12.0/msvc2022_64")
endif()

find_package(Qt6 COMPONENTS Widgets QUIET)
if (Qt6_FOUND)
    add_compile_definitions(POWERMAC_QT_UI)
endif()
if (Qt6_FOUND)
    message(STATUS "Qt6 found: ${Qt6_DIR}")
else()
    message(STATUS "Qt6 not found - building the fallback ImGui VM manager")
endif()

# ----------------------------------------------------------------------------
# Options
# ----------------------------------------------------------------------------
option(DPPC_BUILD_PPC_TESTS  "Build PowerPC tests" OFF)
option(DPPC_68K_DEBUGGER     "Enable 68k debugging" OFF)
option(DPPC_SUPPORT_PPC_LE   "Enable PPC little endian mode" OFF)
option(DPPC_SUPPORT_MEM_LE   "Enable memory controller little endian mode" OFF)

if (DPPC_SUPPORT_PPC_LE)
    add_compile_definitions(SUPPORTS_PPC_LITTLE_ENDIAN_MODE=1)
else()
    add_compile_definitions(SUPPORTS_PPC_LITTLE_ENDIAN_MODE=0)
endif()
if (DPPC_SUPPORT_MEM_LE)
    add_compile_definitions(SUPPORTS_MEMORY_CTRL_ENDIAN_MODE=1)
else()
    add_compile_definitions(SUPPORTS_MEMORY_CTRL_ENDIAN_MODE=0)
endif()

# ----------------------------------------------------------------------------
# Dear ImGui (portable 86Box-style menu bar and configuration dialogs)
# ----------------------------------------------------------------------------
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/thirdparty/imgui")
add_library(imgui STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp"
)
target_include_directories(imgui PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
target_link_libraries(imgui PUBLIC SDL2::SDL2)
target_compile_features(imgui PUBLIC cxx_std_17)

# ----------------------------------------------------------------------------
# cubeb audio back-end
# ----------------------------------------------------------------------------
set(BUILD_TESTS OFF CACHE BOOL "Build Cubeb tests" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(BUILD_TOOLS OFF CACHE BOOL "Build Cubeb tools" FORCE)
set(USE_SANITIZERS OFF CACHE BOOL "Use sanitizers" FORCE)
set(BUNDLE_SPEEX OFF CACHE BOOL "Bundle the speex library" FORCE)
add_subdirectory(thirdparty/cubeb EXCLUDE_FROM_ALL)

# ----------------------------------------------------------------------------
# Include paths shared by the engine and the front-end. This has to happen
# before the engine subdirectories are added so that they inherit them.
# ----------------------------------------------------------------------------
include_directories(
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/core"
    "${CMAKE_SOURCE_DIR}/devices"
    "${CMAKE_SOURCE_DIR}/cpu/ppc"
    "${CMAKE_SOURCE_DIR}/debugger"
    "${CMAKE_SOURCE_DIR}/utils"
    "${CMAKE_SOURCE_DIR}/frontend"
    "${CMAKE_SOURCE_DIR}/thirdparty/loguru"
    "${CMAKE_SOURCE_DIR}/thirdparty/CLI11"
    "${CMAKE_SOURCE_DIR}/thirdparty/cubeb/include"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
)

# ----------------------------------------------------------------------------
# DingusPPC emulation engine
# ----------------------------------------------------------------------------
add_subdirectory(core)
add_subdirectory(cpu/ppc)
add_subdirectory(debugger)
add_subdirectory(devices)
add_subdirectory(machines)
add_subdirectory(utils)
add_subdirectory(thirdparty/loguru)

if (DPPC_68K_DEBUGGER)
    add_subdirectory(thirdparty/capstone EXCLUDE_FROM_ALL)
endif()

# ----------------------------------------------------------------------------
# Front-end executable
# ----------------------------------------------------------------------------
platform_glob(APP_SOURCES
    ${CMAKE_SOURCE_DIR}/*.cpp
    ${CMAKE_SOURCE_DIR}/*.h
)
list(APPEND APP_SOURCES
    ${CMAKE_SOURCE_DIR}/frontend/config.cpp
    ${CMAKE_SOURCE_DIR}/frontend/ui.cpp
    ${CMAKE_SOURCE_DIR}/frontend/runtime.cpp
)
if (Qt6_FOUND)
    list(APPEND APP_SOURCES ${CMAKE_SOURCE_DIR}/frontend/qt_ui.cpp)
endif()
if (APPLE)
    platform_glob(APPLE_SOURCES "${CMAKE_SOURCE_DIR}/*.m")
    list(APPEND APP_SOURCES ${APPLE_SOURCES})
endif()

add_executable(PowerMacEmulator ${APP_SOURCES}
    $<TARGET_OBJECTS:core>
    $<TARGET_OBJECTS:cpu_ppc>
    $<TARGET_OBJECTS:debugger>
    $<TARGET_OBJECTS:devices>
    $<TARGET_OBJECTS:machines>
    $<TARGET_OBJECTS:utils>
    $<TARGET_OBJECTS:loguru>
)

target_link_libraries(PowerMacEmulator PRIVATE imgui cubeb)

if (WIN32)
    target_link_libraries(PowerMacEmulator PRIVATE SDL2::SDL2)
    target_compile_definitions(PowerMacEmulator PRIVATE SDL_MAIN_HANDLED)
elseif (EMSCRIPTEN)
    target_link_libraries(PowerMacEmulator PRIVATE ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT})
else()
    target_link_libraries(PowerMacEmulator PRIVATE SDL2::SDL2main SDL2::SDL2
        ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT})
endif()

if (DPPC_68K_DEBUGGER)
    target_link_libraries(PowerMacEmulator PRIVATE capstone)
endif()

set_target_properties(PowerMacEmulator PROPERTIES
    OUTPUT_NAME "PowerMacEmulator"
)

if (Qt6_FOUND)
    target_link_libraries(PowerMacEmulator PRIVATE Qt6::Widgets)
    target_compile_definitions(PowerMacEmulator PRIVATE POWERMAC_QT_UI)
    if (WIN32)
        find_program(WINDEPLOYQT_EXE windeployqt)
        if (WINDEPLOYQT_EXE)
            add_custom_command(TARGET PowerMacEmulator POST_BUILD
                COMMAND "${WINDEPLOYQT_EXE}" --no-translations
                        --dir "$<TARGET_FILE_DIR:PowerMacEmulator>"
                        "$<TARGET_FILE:PowerMacEmulator>"
                COMMENT "Deploying Qt runtime for PowerMacEmulator")
        endif()
    endif()
endif()

# Copy the SDL2 runtime next to the executable so it can be started directly.
if (WIN32)
    add_custom_command(TARGET PowerMacEmulator POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL2::SDL2>"
            "$<TARGET_FILE_DIR:PowerMacEmulator>"
        VERBATIM)
endif()

install(TARGETS PowerMacEmulator RUNTIME DESTINATION bin)

# ----------------------------------------------------------------------------
# Rhapsody-specific executable (G3 + IDE profile)
# ----------------------------------------------------------------------------
add_executable(RhapsodyEmulator ${APP_SOURCES}
    $<TARGET_OBJECTS:core>
    $<TARGET_OBJECTS:cpu_ppc>
    $<TARGET_OBJECTS:debugger>
    $<TARGET_OBJECTS:devices>
    $<TARGET_OBJECTS:machines>
    $<TARGET_OBJECTS:utils>
    $<TARGET_OBJECTS:loguru>
)

target_compile_definitions(RhapsodyEmulator PRIVATE POWERMAC_RHAPSODY)
target_link_libraries(RhapsodyEmulator PRIVATE imgui cubeb)

if (WIN32)
    target_link_libraries(RhapsodyEmulator PRIVATE SDL2::SDL2)
    target_compile_definitions(RhapsodyEmulator PRIVATE SDL_MAIN_HANDLED)
elseif (EMSCRIPTEN)
    target_link_libraries(RhapsodyEmulator PRIVATE ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT})
else()
    target_link_libraries(RhapsodyEmulator PRIVATE SDL2::SDL2main SDL2::SDL2
        ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT})
endif()

if (DPPC_68K_DEBUGGER)
    target_link_libraries(RhapsodyEmulator PRIVATE capstone)
endif()

set_target_properties(RhapsodyEmulator PROPERTIES
    OUTPUT_NAME "RhapsodyEmulator"
)

if (Qt6_FOUND)
    target_link_libraries(RhapsodyEmulator PRIVATE Qt6::Widgets)
    target_compile_definitions(RhapsodyEmulator PRIVATE POWERMAC_QT_UI)
    if (WIN32)
        find_program(WINDEPLOYQT_EXE windeployqt)
        if (WINDEPLOYQT_EXE)
            add_custom_command(TARGET RhapsodyEmulator POST_BUILD
                COMMAND "${WINDEPLOYQT_EXE}" --no-translations
                        --dir "$<TARGET_FILE_DIR:RhapsodyEmulator>"
                        "$<TARGET_FILE:RhapsodyEmulator>"
                COMMENT "Deploying Qt runtime for RhapsodyEmulator")
        endif()
    endif()
endif()

if (WIN32)
    add_custom_command(TARGET RhapsodyEmulator POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL2::SDL2>"
            "$<TARGET_FILE_DIR:RhapsodyEmulator>"
        VERBATIM)
endif()

target_compile_definitions(RhapsodyEmulator PRIVATE
    POWERMAC_PROJECT_DIR="${CMAKE_SOURCE_DIR}")

install(TARGETS RhapsodyEmulator RUNTIME DESTINATION bin)

# ----------------------------------------------------------------------------
# Standalone virtual machine manager
# ----------------------------------------------------------------------------
if (Qt6_FOUND)
    add_executable(PowerMacVMManager
        "${CMAKE_SOURCE_DIR}/frontend/qt_vmmanager.cpp"
        "${CMAKE_SOURCE_DIR}/frontend/vmmanager.cpp"
        "${CMAKE_SOURCE_DIR}/utils/imgfile_sdl.cpp"
        $<TARGET_OBJECTS:loguru>
    )
    target_link_libraries(PowerMacVMManager PRIVATE Qt6::Widgets)

    if (WIN32)
        target_link_options(PowerMacVMManager PRIVATE
            /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    endif()

    if (WIN32)
        find_program(WINDEPLOYQT_EXE windeployqt)
        if (WINDEPLOYQT_EXE)
            add_custom_command(TARGET PowerMacVMManager POST_BUILD
                COMMAND "${WINDEPLOYQT_EXE}" --no-translations
                        --dir "$<TARGET_FILE_DIR:PowerMacVMManager>"
                        "$<TARGET_FILE:PowerMacVMManager>"
                COMMENT "Deploying Qt runtime for PowerMacVMManager")
        endif()
    endif()
else()
    add_executable(PowerMacVMManager
        "${CMAKE_SOURCE_DIR}/frontend/vmmanager_main.cpp"
        "${CMAKE_SOURCE_DIR}/frontend/vmmanager.cpp"
        "${CMAKE_SOURCE_DIR}/utils/imgfile_sdl.cpp"
        $<TARGET_OBJECTS:loguru>
    )
    target_link_libraries(PowerMacVMManager PRIVATE imgui SDL2::SDL2)
    target_compile_definitions(PowerMacVMManager PRIVATE SDL_MAIN_HANDLED)

    if (WIN32)
        add_custom_command(TARGET PowerMacVMManager POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:SDL2::SDL2>"
                "$<TARGET_FILE_DIR:PowerMacVMManager>"
            VERBATIM)
    endif()
endif()

install(TARGETS PowerMacVMManager RUNTIME DESTINATION bin)

# Project directory baked into the emulator so it can launch the standalone
# VM manager with the correct working directory and VM root.
target_compile_definitions(PowerMacEmulator PRIVATE
    POWERMAC_PROJECT_DIR="${CMAKE_SOURCE_DIR}")
