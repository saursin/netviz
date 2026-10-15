# All third-party dependencies are declared here, pinned to exact commits.
#
# To build offline or against a local checkout, point CMake at the source:
#   -DFETCHCONTENT_SOURCE_DIR_SIMDJSON=/path/to/simdjson
#   -DFETCHCONTENT_SOURCE_DIR_IMGUI=/path/to/imgui
#   -DFETCHCONTENT_SOURCE_DIR_SDL3=/path/to/SDL
#   -DFETCHCONTENT_SOURCE_DIR_DOCTEST=/path/to/doctest
# or set FETCHCONTENT_FULLY_DISCONNECTED=ON once sources are downloaded.

include(FetchContent)

# ---------------------------------------------------------------------------
# Netlist import
# ---------------------------------------------------------------------------
# simdjson: fast, order-preserving JSON parser for Yosys netlists.
FetchContent_Declare(simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG        f5de14f09256982933af2849beb43778bd421ca7 # v4.6.11
    GIT_SHALLOW    FALSE
    SYSTEM)
FetchContent_MakeAvailable(simdjson)

# ---------------------------------------------------------------------------
# GUI dependencies
# ---------------------------------------------------------------------------
if(NETVIZ_BUILD_GUI)
    # SDL3: prefer the system package, fall back to building from source.
    if(NOT NETVIZ_FETCH_SDL3)
        find_package(SDL3 3.2 CONFIG QUIET)
    endif()
    if(SDL3_FOUND)
        message(STATUS "netviz: using system SDL3 ${SDL3_VERSION}")
    else()
        message(STATUS "netviz: fetching SDL3")
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON  CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(sdl3
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG        fa2c02bb6e21974a89ea9824bc53c9932abe5f9c # release-3.4.16
            GIT_SHALLOW    FALSE
            SYSTEM)
        FetchContent_MakeAvailable(sdl3)
    endif()

    find_package(OpenGL REQUIRED)

    # Dear ImGui, docking branch. ImGui has no CMake build of its own,
    # so we compile it here as a static library with the backends we use.
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        b916186e66aa3c02b72c6ca17455516512ed8049 # v1.93.0-docking
        GIT_SHALLOW    FALSE)
    FetchContent_MakeAvailable(imgui)

    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
    target_include_directories(imgui SYSTEM PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends)
    target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    target_link_libraries(imgui PUBLIC SDL3::SDL3 OpenGL::GL)
endif()

# ---------------------------------------------------------------------------
# Test dependencies
# ---------------------------------------------------------------------------
if(NETVIZ_BUILD_TESTS)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG        2d0a9359a60c51affe2a9bebb1be1dca47868151 # v2.5.3
        GIT_SHALLOW    FALSE
        SYSTEM)
    set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(doctest)
endif()
