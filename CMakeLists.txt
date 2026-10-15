cmake_minimum_required(VERSION 3.24)

project(netviz
    VERSION 0.1.0
    DESCRIPTION "Interactive netlist visualizer"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(NETVIZ_BUILD_GUI   "Build the GUI application (needs SDL3, OpenGL, ImGui)" ON)
option(NETVIZ_BUILD_TESTS "Build unit tests" ${PROJECT_IS_TOP_LEVEL})
option(NETVIZ_FETCH_SDL3  "Always fetch SDL3 instead of using the system package" OFF)
option(NETVIZ_WERROR      "Treat compiler warnings as errors" OFF)

include(GNUInstallDirs)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(Dependencies)
include(EmbedFile)

# netviz_set_warnings(<target>)
# Applies the project warning set to one of our own targets.
# Third-party targets are left alone.
function(netviz_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(NETVIZ_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
        if(NETVIZ_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

add_subdirectory(src)

if(NETVIZ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
