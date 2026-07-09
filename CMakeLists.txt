cmake_minimum_required(VERSION 3.16)
project(slims3 VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SLIMS3_BUILD_TESTS "Build slim-s3 tests" OFF)

find_package(CURL REQUIRED)

add_library(slims3
    src/client.cpp
    src/sha256.cpp
    src/uri.cpp
    src/signer.cpp
    src/xml_lite.cpp
    src/transport.cpp
)
add_library(slims3::slims3 ALIAS slims3)
target_include_directories(slims3 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(slims3 PUBLIC CURL::libcurl)
target_compile_features(slims3 PUBLIC cxx_std_17)
# Feeds the default User-Agent ("slim-s3/<version>"); see transport.cpp.
target_compile_definitions(slims3 PRIVATE SLIMS3_VERSION="${PROJECT_VERSION}")
if(NOT MSVC)
    target_compile_options(slims3 PRIVATE -Wall -Wextra)
endif()

include(GNUInstallDirs)
install(TARGETS slims3 EXPORT slims3-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/slims3 DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT slims3-targets NAMESPACE slims3:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/slims3)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/slims3Config.cmake
    "include(CMakeFindDependencyMacro)\nfind_dependency(CURL)\ninclude(\"\${CMAKE_CURRENT_LIST_DIR}/slims3-targets.cmake\")\n")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/slims3Config.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/slims3)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/slims3ConfigVersion.cmake"
    COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/slims3ConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/slims3)

if(SLIMS3_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
