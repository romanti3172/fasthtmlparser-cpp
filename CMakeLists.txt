cmake_minimum_required(VERSION 3.20)

project(fasthtmlparser
    VERSION 0.1.4
    DESCRIPTION "Fast HTML / XML parser for scraping with XPath 1.0 and CSS selectors (a C++20 port of z3l.FastHtmlParser)"
    HOMEPAGE_URL "https://github.com/romanti3172/fasthtmlparser-cpp"
    LANGUAGES CXX)

if(NOT DEFINED PROJECT_IS_TOP_LEVEL)
    string(COMPARE EQUAL "${CMAKE_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}" PROJECT_IS_TOP_LEVEL)
endif()

option(FASTHTMLPARSER_BUILD_TESTS "Build the tests (fetches doctest)" ${PROJECT_IS_TOP_LEVEL})
option(FASTHTMLPARSER_INSTALL "Generate the install target" ON)

include(GNUInstallDirs)

add_library(fasthtmlparser
    src/attributes.cpp
    src/chars.cpp
    src/css.cpp
    src/decode.cpp
    src/document.cpp
    src/links.cpp
    src/parse.cpp
    src/text.cpp
    src/xpath.cpp)
add_library(fasthtmlparser::fasthtmlparser ALIAS fasthtmlparser)

target_compile_features(fasthtmlparser PUBLIC cxx_std_20)
target_include_directories(fasthtmlparser
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(fasthtmlparser PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    WINDOWS_EXPORT_ALL_SYMBOLS ON)

if(MSVC)
    target_compile_options(fasthtmlparser PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
else()
    target_compile_options(fasthtmlparser PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Optional link-time and profile-guided optimization of the library, off by default (see "Faster builds" in the
# README): -DFASTHTMLPARSER_LTO=ON, and -DFASTHTMLPARSER_PGO=GENERATE, a run of your program on your pages, then
# -DFASTHTMLPARSER_PGO=USE.
option(FASTHTMLPARSER_LTO "Link-time optimization of the library" OFF)
set(FASTHTMLPARSER_PGO OFF CACHE STRING "Profile-guided optimization of the library: OFF, GENERATE or USE")
set_property(CACHE FASTHTMLPARSER_PGO PROPERTY STRINGS OFF GENERATE USE)
set(FASTHTMLPARSER_PGO_DIR "${CMAKE_BINARY_DIR}/pgo" CACHE PATH
    "Where GCC and Clang write and read the profile (MSVC keeps it next to each program)")
include(cmake/optimize.cmake)

if(FASTHTMLPARSER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(FASTHTMLPARSER_INSTALL)
    include(CMakePackageConfigHelpers)
    install(TARGETS fasthtmlparser
        EXPORT fasthtmlparserTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(DIRECTORY include/fasthtmlparser DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT fasthtmlparserTargets
        NAMESPACE fasthtmlparser::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fasthtmlparser)
    configure_package_config_file(cmake/fasthtmlparserConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/fasthtmlparserConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fasthtmlparser)
    write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/fasthtmlparserConfigVersion.cmake
        COMPATIBILITY SameMinorVersion)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/fasthtmlparserConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/fasthtmlparserConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fasthtmlparser)
    install(FILES LICENSE.md DESTINATION ${CMAKE_INSTALL_DATADIR}/fasthtmlparser)
endif()
