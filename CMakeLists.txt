cmake_minimum_required(VERSION 3.23)
project(nref LANGUAGES CXX)

add_library(nref INTERFACE)
target_include_directories(nref INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(nref INTERFACE cxx_std_20)

file(GLOB_RECURSE NREF_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/nrx/ref/*.hpp"
)
target_sources(nref INTERFACE
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES ${NREF_HEADERS}
)

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
)

enable_testing()
foreach(test_src IN LISTS TEST_SOURCES)
    get_filename_component(test_name "${test_src}" NAME_WE)
    add_executable("${test_name}" "${test_src}")
    target_link_libraries("${test_name}" PRIVATE nref)
    add_test(NAME "${test_name}" COMMAND "${test_name}")
endforeach()
