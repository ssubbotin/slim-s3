add_executable(slims3_tests
    test_main.cpp
    test_smoke.cpp
    test_sha256.cpp
    test_uri.cpp
    test_signer.cpp
    test_xml.cpp
    test_transport.cpp
    test_client_mapping.cpp
    test_header_validation.cpp
)
target_link_libraries(slims3_tests PRIVATE slims3)
target_include_directories(slims3_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src)
add_test(NAME unit COMMAND slims3_tests)

add_executable(slims3_itest integration/itest_main.cpp)
target_link_libraries(slims3_itest PRIVATE slims3)
target_include_directories(slims3_itest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME integration COMMAND slims3_itest)
set_tests_properties(integration PROPERTIES LABELS integration)
