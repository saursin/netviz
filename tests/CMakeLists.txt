add_executable(netviz_tests
    test_core.cc
    test_netlist.cc
    test_yosys_json.cc
    test_display_graph.cc
    test_view.cc)
target_link_libraries(netviz_tests PRIVATE netviz_view netviz_io doctest::doctest_with_main)
netviz_set_warnings(netviz_tests)
target_compile_definitions(netviz_tests PRIVATE NETVIZ_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/ex")

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(netviz_tests)
