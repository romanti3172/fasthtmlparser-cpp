# Benchmarks of fasthtmlparser against lexbor, Gumbo and libxml2. Its own project: the benchmark dependencies
# stay out of the library. (myhtml, lexbor's deprecated predecessor, is left out: its CMakeLists needs policies that
# CMake 4 no longer has.)
#
#     cmake -S bench -B bench/build -G Ninja -DCMAKE_BUILD_TYPE=Release
#     cmake --build bench/build && bench/build/fasthtmlparser_bench
cmake_minimum_required(VERSION 3.20)
project(fasthtmlparser_bench LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(BENCH_LIBXML2 "Also benchmark libxml2 (HTML parser and XPath)" ON)

# old CMakeLists of the dependencies
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(BUILD_SHARED_LIBS OFF)

add_subdirectory(.. fasthtmlparser EXCLUDE_FROM_ALL)

include(FetchContent)

# lexbor: a fast HTML5 parser in C with CSS selectors (behind selectolax and PHP 8.4's Dom\HTMLDocument)
set(LEXBOR_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_STATIC ON CACHE BOOL "" FORCE)
FetchContent_Declare(lexbor
    GIT_REPOSITORY https://github.com/lexbor/lexbor.git
    GIT_TAG v3.0.1
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(lexbor)

# Gumbo: Google's HTML5 parser in C (the maintained fork); built here from its sources
FetchContent_Declare(gumbo
    GIT_REPOSITORY https://codeberg.org/gumbo-parser/gumbo-parser.git
    GIT_TAG 0.14.1
    GIT_SHALLOW TRUE)
FetchContent_GetProperties(gumbo)
if(NOT gumbo_POPULATED)
    FetchContent_Populate(gumbo)
endif()
add_library(gumbo STATIC
    ${gumbo_SOURCE_DIR}/src/attribute.c
    ${gumbo_SOURCE_DIR}/src/char_ref.c
    ${gumbo_SOURCE_DIR}/src/char_ref_gperf.c
    ${gumbo_SOURCE_DIR}/src/error.c
    ${gumbo_SOURCE_DIR}/src/parser.c
    ${gumbo_SOURCE_DIR}/src/string_buffer.c
    ${gumbo_SOURCE_DIR}/src/string_piece.c
    ${gumbo_SOURCE_DIR}/src/tag.c
    ${gumbo_SOURCE_DIR}/src/tokenizer.c
    ${gumbo_SOURCE_DIR}/src/utf8.c
    ${gumbo_SOURCE_DIR}/src/util.c
    ${gumbo_SOURCE_DIR}/src/vector.c)
target_include_directories(gumbo PUBLIC ${gumbo_SOURCE_DIR}/src)
if(MSVC)
    target_include_directories(gumbo PRIVATE ${gumbo_SOURCE_DIR}/visualc/include)
    target_compile_definitions(gumbo PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(gumbo PRIVATE /w)
else()
    target_compile_options(gumbo PRIVATE -w -std=gnu99)
endif()

add_executable(fasthtmlparser_bench bench.cpp)
target_link_libraries(fasthtmlparser_bench PRIVATE fasthtmlparser::fasthtmlparser lexbor_static gumbo)
target_compile_definitions(fasthtmlparser_bench PRIVATE
    LEXBOR_STATIC
    TESTDATA="${CMAKE_CURRENT_SOURCE_DIR}/../testdata")
if(MSVC)
    target_compile_options(fasthtmlparser_bench PRIVATE /utf-8 /W3)
endif()

if(BENCH_LIBXML2)
    # libxml2: the parser behind lxml and Nokogiri; its HTML parser and XPath
    foreach(opt ICONV ICU LZMA ZLIB PYTHON PROGRAMS TESTS HTTP FTP DEBUG READLINE HISTORY MODULES THREADS)
        set(LIBXML2_WITH_${opt} OFF CACHE BOOL "" FORCE)
    endforeach()
    FetchContent_Declare(libxml2
        GIT_REPOSITORY https://github.com/GNOME/libxml2.git
        GIT_TAG v2.15.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(libxml2)
    target_link_libraries(fasthtmlparser_bench PRIVATE LibXml2::LibXml2)
    target_compile_definitions(fasthtmlparser_bench PRIVATE BENCH_LIBXML2 LIBXML_STATIC)
endif()

option(BENCH_SANBLCH "Also benchmark sanblch/fasthtmlparser-cpp, an independent C++ port" ON)
if(BENCH_SANBLCH)
    # sanblch/fasthtmlparser-cpp: another C++20 port of z3l.FastHtmlParser by a reader of the Habr article. Its
    # CMakeLists is not used: its project has our name and builds its own tests and benchmark. The library is built
    # here from its sources, with the flags of its CMakeLists (C++20, /utf-8), as the target sanblch_fh.
    FetchContent_Declare(sanblch
        GIT_REPOSITORY https://github.com/sanblch/fasthtmlparser-cpp.git
        GIT_TAG 2fb1d7a2599570a6b4a7c4b3951036ea48e72065)
    FetchContent_GetProperties(sanblch)
    if(NOT sanblch_POPULATED)
        FetchContent_Populate(sanblch)
    endif()
    add_library(sanblch_fh STATIC
        ${sanblch_SOURCE_DIR}/src/tables.cpp
        ${sanblch_SOURCE_DIR}/src/decode.cpp
        ${sanblch_SOURCE_DIR}/src/text.cpp
        ${sanblch_SOURCE_DIR}/src/parse.cpp
        ${sanblch_SOURCE_DIR}/src/document.cpp
        ${sanblch_SOURCE_DIR}/src/uri.cpp
        ${sanblch_SOURCE_DIR}/src/css.cpp
        ${sanblch_SOURCE_DIR}/src/xpath.cpp
        ${sanblch_SOURCE_DIR}/src/generated_entities.cpp)
    target_compile_features(sanblch_fh PUBLIC cxx_std_20)
    target_include_directories(sanblch_fh PUBLIC ${sanblch_SOURCE_DIR}/include PRIVATE ${sanblch_SOURCE_DIR}/src)
    if(MSVC)
        target_compile_options(sanblch_fh PRIVATE /utf-8 /w)
    else()
        target_compile_options(sanblch_fh PRIVATE -w)
    endif()
    target_link_libraries(fasthtmlparser_bench PRIVATE sanblch_fh)
    target_compile_definitions(fasthtmlparser_bench PRIVATE BENCH_SANBLCH)
endif()
