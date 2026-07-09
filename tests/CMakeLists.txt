add_executable(slims3_tests
    test_main.cpp
    test_smoke.cpp
    test_sha256.cpp
    test_uri.cpp
)
target_link_libraries(slims3_tests PRIVATE slims3)
target_include_directories(slims3_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src)
add_test(NAME unit COMMAND slims3_tests)
