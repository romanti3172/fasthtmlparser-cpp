# Link-time (FASTHTMLPARSER_LTO) and profile-guided (FASTHTMLPARSER_PGO) optimization of the target fasthtmlparser.
#
# A static library is optimized when the program that uses it is linked, so its link options go to that program
# (INTERFACE, only in this build tree: an installed package carries none of them); a shared library is linked here.

string(TOUPPER "${FASTHTMLPARSER_PGO}" _fhp_pgo)
if(_fhp_pgo STREQUAL "" OR _fhp_pgo MATCHES "^(OFF|NO|FALSE|0)$")
    set(_fhp_pgo OFF)
elseif(NOT _fhp_pgo MATCHES "^(GENERATE|USE)$")
    message(FATAL_ERROR "FASTHTMLPARSER_PGO must be OFF, GENERATE or USE, not '${FASTHTMLPARSER_PGO}'")
endif()

get_target_property(_fhp_type fasthtmlparser TYPE)
if(_fhp_type STREQUAL "STATIC_LIBRARY")
    set(_fhp_link INTERFACE)
else()
    set(_fhp_link PRIVATE)
endif()

function(_fhp_link_options)
    foreach(opt IN LISTS ARGN)
        target_link_options(fasthtmlparser ${_fhp_link} "$<BUILD_INTERFACE:${opt}>")
    endforeach()
endfunction()

set(_fhp_msvc_pgo OFF)
if(NOT _fhp_pgo STREQUAL "OFF")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        set(_fhp_msvc_pgo ON)
    elseif(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$" OR MSVC)
        message(WARNING "FASTHTMLPARSER_PGO: not supported with ${CMAKE_CXX_COMPILER_ID} (MSVC, GCC and Clang are); "
                        "building without it")
        set(_fhp_pgo OFF)
    endif()
endif()

# LTO; MSVC's PGO needs it (/GL and /LTCG)
if(FASTHTMLPARSER_LTO OR _fhp_msvc_pgo)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _fhp_ipo OUTPUT _fhp_ipo_output LANGUAGES CXX)
    if(_fhp_ipo)
        set_property(TARGET fasthtmlparser PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
        if(_fhp_link STREQUAL "INTERFACE")
            if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
                _fhp_link_options(/LTCG)
            else()
                _fhp_link_options(${CMAKE_CXX_COMPILE_OPTIONS_IPO})
            endif()
        endif()
        message(STATUS "fasthtmlparser: link-time optimization on")
    else()
        message(WARNING "fasthtmlparser: link-time optimization is not supported here, building without it "
                        "(and without PGO under MSVC): ${_fhp_ipo_output}")
        set(_fhp_msvc_pgo OFF)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            set(_fhp_pgo OFF)
        endif()
    endif()
endif()

if(_fhp_pgo STREQUAL "OFF")
    return()
endif()

if(_fhp_msvc_pgo)
    # The profile is <program>.pgd next to each program; a run writes <program>!N.pgc files, which the USE link merges.
    # An instrumented program needs pgort140.dll on the PATH (a developer prompt has it).
    if(_fhp_pgo STREQUAL "GENERATE")
        _fhp_link_options(/GENPROFILE)
    else()
        _fhp_link_options(/USEPROFILE)
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # One .gcda file per object in FASTHTMLPARSER_PGO_DIR, named after the object's path (GENERATE and USE must use
    # the same build directory); -fprofile-prefix-path keeps the build directory out of that name, which on Windows
    # would otherwise go past MAX_PATH, and the files would silently not be written.
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12)
        cmake_path(NATIVE_PATH CMAKE_BINARY_DIR _fhp_prefix) # GCC compares it with backslashes on Windows
        target_compile_options(fasthtmlparser PRIVATE "-fprofile-prefix-path=${_fhp_prefix}")
    endif()
    if(_fhp_pgo STREQUAL "GENERATE")
        # atomic counters: a program that parses on several threads would otherwise write a broken profile
        target_compile_options(fasthtmlparser PRIVATE "-fprofile-generate=${FASTHTMLPARSER_PGO_DIR}"
                               -fprofile-update=prefer-atomic)
        _fhp_link_options("-fprofile-generate=${FASTHTMLPARSER_PGO_DIR}")
    else()
        target_compile_options(fasthtmlparser PRIVATE "-fprofile-use=${FASTHTMLPARSER_PGO_DIR}"
                               -fprofile-partial-training -Wno-missing-profile)
    endif()
else()
    # Clang: a run writes *.profraw into FASTHTMLPARSER_PGO_DIR; configuring with USE merges them with llvm-profdata.
    set(_fhp_profdata "${FASTHTMLPARSER_PGO_DIR}/fasthtmlparser.profdata")
    if(_fhp_pgo STREQUAL "GENERATE")
        # atomic counters: a program that parses on several threads would otherwise write a broken profile
        target_compile_options(fasthtmlparser PRIVATE "-fprofile-generate=${FASTHTMLPARSER_PGO_DIR}"
                               -fprofile-update=prefer-atomic)
        _fhp_link_options("-fprofile-generate=${FASTHTMLPARSER_PGO_DIR}")
    else()
        file(GLOB _fhp_raw "${FASTHTMLPARSER_PGO_DIR}/*.profraw")
        if(_fhp_raw)
            string(REGEX MATCH "^[0-9]+" _fhp_clang_major "${CMAKE_CXX_COMPILER_VERSION}")
            get_filename_component(_fhp_clang_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
            find_program(FASTHTMLPARSER_LLVM_PROFDATA NAMES llvm-profdata-${_fhp_clang_major} llvm-profdata
                         HINTS "${_fhp_clang_dir}")
            if(NOT FASTHTMLPARSER_LLVM_PROFDATA)
                message(FATAL_ERROR "FASTHTMLPARSER_PGO=USE: llvm-profdata not found (set FASTHTMLPARSER_LLVM_PROFDATA)")
            endif()
            execute_process(COMMAND "${FASTHTMLPARSER_LLVM_PROFDATA}" merge -o "${_fhp_profdata}" ${_fhp_raw}
                            RESULT_VARIABLE _fhp_merge)
            if(NOT _fhp_merge EQUAL 0)
                message(FATAL_ERROR "FASTHTMLPARSER_PGO=USE: llvm-profdata merge failed")
            endif()
        endif()
        if(NOT EXISTS "${_fhp_profdata}")
            message(FATAL_ERROR "FASTHTMLPARSER_PGO=USE: no profile in ${FASTHTMLPARSER_PGO_DIR}; build with GENERATE "
                                "and run your program first")
        endif()
        target_compile_options(fasthtmlparser PRIVATE "-fprofile-use=${_fhp_profdata}"
                               -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date)
    endif()
endif()
message(STATUS "fasthtmlparser: profile-guided optimization, ${_fhp_pgo}")
