add_executable(netviz_tests
    test_core.cc
    test_view.cc)
target_link_libraries(netviz_tests PRIVATE netviz_view doctest::doctest_with_main)
netviz_set_warnings(netviz_tests)

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(netviz_tests)
