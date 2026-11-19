include(FetchContent)
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.12
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(doctest)

add_executable(fasthtmlparser_tests
    main.cpp
    api_test.cpp
    conformance_test.cpp)
target_link_libraries(fasthtmlparser_tests PRIVATE fasthtmlparser::fasthtmlparser doctest::doctest)
# the conformance test reads the inner positions of the tags from the private header
target_include_directories(fasthtmlparser_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(fasthtmlparser_tests PRIVATE
    FASTHTMLPARSER_TESTDATA="${PROJECT_SOURCE_DIR}/testdata")
if(MSVC)
    target_compile_options(fasthtmlparser_tests PRIVATE /W4 /permissive- /utf-8 /bigobj)
else()
    target_compile_options(fasthtmlparser_tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME fasthtmlparser_tests COMMAND fasthtmlparser_tests)
