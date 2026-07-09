cmake_minimum_required(VERSION 3.16)
project(slims3_test_package CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(slims3 CONFIG REQUIRED)

add_executable(example example.cpp)
target_link_libraries(example PRIVATE slims3::slims3)
