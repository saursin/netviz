# Library layering (each library may only depend on the ones above it):
#
#   netviz_core      std only: logging, commands, netlist IR + connectivity
#   netviz_io        core + simdjson: file importers (Yosys JSON)
#   netviz_view      core: camera, display graph, spatial grid
#   netviz_layout    view: layout contract and the layered layout engine
#   netviz_gui       view + io + SDL3 / OpenGL / ImGui: window, panels, app UI
#
# core and view must never include ImGui, SDL or OpenGL headers.

add_library(netviz_core
    netviz/core/command.cc
    netviz/core/connectivity.cc
    netviz/core/log.cc
    netviz/core/netlist.cc)
target_include_directories(netviz_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(netviz_core PUBLIC cxx_std_20)
netviz_set_warnings(netviz_core)

add_library(netviz_io
    netviz/io/yosys_json.cc)
target_link_libraries(netviz_io PUBLIC netviz_core PRIVATE simdjson::simdjson)
netviz_set_warnings(netviz_io)

add_library(netviz_view
    netviz/view/camera.cc
    netviz/view/display_graph.cc
    netviz/view/spatial_grid.cc)
target_link_libraries(netviz_view PUBLIC netviz_core)
netviz_set_warnings(netviz_view)

add_library(netviz_layout
    netviz/layout/layered_layout.cc)
target_link_libraries(netviz_layout PUBLIC netviz_view)
netviz_set_warnings(netviz_layout)

if(NETVIZ_BUILD_GUI)
    add_library(netviz_gui
        netviz/gui/window.cc
        netviz/gui/app_ui.cc
        netviz/gui/canvas.cc
        netviz/gui/console.cc
        netviz/gui/design_panels.cc
        netviz/gui/preferences.cc)
    set(fonts ${PROJECT_SOURCE_DIR}/third_party)
    netviz_embed_file(netviz_gui ${fonts}/inter/Inter-Regular.ttf inter_ttf)
    netviz_embed_file(netviz_gui ${fonts}/jetbrains-mono/JetBrainsMono-Regular.ttf jetbrains_mono_ttf)
    netviz_embed_file(netviz_gui ${fonts}/lucide/lucide.ttf lucide_ttf)
    target_link_libraries(netviz_gui PUBLIC netviz_view netviz_io imgui)
    if(WIN32)
        target_link_libraries(netviz_gui PRIVATE psapi) # GetProcessMemoryInfo
    endif()
    target_compile_definitions(netviz_gui PUBLIC NETVIZ_VERSION="${PROJECT_VERSION}")
    netviz_set_warnings(netviz_gui)

    add_executable(netviz main.cc)
    target_link_libraries(netviz PRIVATE netviz_gui)
    netviz_set_warnings(netviz)
    # Put the binary at the top of the build directory: build/<preset>/netviz
    set_target_properties(netviz PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
    install(TARGETS netviz)
endif()
