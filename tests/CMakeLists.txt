add_executable(netviz_tests
    test_core.cc
    test_netlist.cc
    test_yosys_json.cc
    test_display_graph.cc
    test_layout.cc
    test_view.cc
    test_settings.cc
    test_rules.cc
    # The preferences file and rules have no GUI dependencies; test them
    # without the GUI library.
    ${PROJECT_SOURCE_DIR}/src/netviz/gui/settings_file.cc
    ${PROJECT_SOURCE_DIR}/src/netviz/gui/rules.cc)
target_link_libraries(netviz_tests PRIVATE netviz_layout netviz_io simdjson::simdjson
                                           doctest::doctest_with_main)
netviz_set_warnings(netviz_tests)
target_compile_definitions(netviz_tests PRIVATE NETVIZ_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/ex")

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(netviz_tests)
