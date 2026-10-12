# SPDX-License-Identifier: GPL-3.0-only
##
# @file CMakeLists.txt
#
# @copyright Copyright (C) 2013-2024 srcML, LLC. (www.srcML.org)

cmake_minimum_required(VERSION 3.20)

# Project version lives in VERSION (also used by benchmark/ for baseline versioning)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SRCSLICE_VERSION)
string(STRIP "${SRCSLICE_VERSION}" SRCSLICE_VERSION)
project(srcSlice VERSION ${SRCSLICE_VERSION})

# enable ctest
enable_testing()

# find needed libraries
find_package(LibXml2 REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS "-Wno-reorder -Wunused-variable -Wunused-parameter -Wnarrowing -Werror=narrowing -Wno-error=all")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include_directories(${LIBXML2_INCLUDE_DIR}
                    ${CMAKE_SOURCE_DIR}/srcDispatch/srcSAX/src/srcsax
                    ${CMAKE_SOURCE_DIR}/srcDispatch/srcSAX/src/cpp
                    ${CMAKE_SOURCE_DIR}/srcDispatch/srcSAX/src/windows
                    external
                    srcDispatch/src/dispatcher
                    srcDispatch/src/policy_classes
                    src/headers)

# Sanitizer builds for the unit tests, e.g.
#   cmake -S . -B build-tsan -G Ninja -DSRCSLICE_SANITIZER=thread -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build build-tsan -j 8 && ctest --test-dir build-tsan -L srcslice
# (an instrumented compile of the handler peaks at ~1.3 GB, so keep -j small on a desktop)
# Applied before the subdirectories so srcDispatch/srcSAX are instrumented as well.
set(SRCSLICE_SANITIZER "" CACHE STRING "Build with -fsanitize=<address|thread|undefined> (empty = off)")
if(SRCSLICE_SANITIZER)
    add_compile_options(-fsanitize=${SRCSLICE_SANITIZER} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${SRCSLICE_SANITIZER})
endif()

add_subdirectory(src)
add_subdirectory(test)
add_subdirectory(srcDispatch)

# Output-regression tests: slice a pinned real codebase and compare the result with the
# golden output recorded in benchmark/golden/ (see benchmark/README.md). The first run of
# each size fetches and converts its input, so it needs network access and srcml.
set(SRCSLICE_REGRESSION_SIZES "small" CACHE STRING
    "benchmark inputs registered as ctest output-regression tests (semicolon list; empty disables)")
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    foreach(SIZE IN LISTS SRCSLICE_REGRESSION_SIZES)
        add_test(NAME regression-${SIZE}
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/benchmark/bench.py
                         --verify --sizes ${SIZE} --srcslice $<TARGET_FILE:srcslice>)
        set_tests_properties(regression-${SIZE} PROPERTIES LABELS "regression" TIMEOUT 7200)
    endforeach()
endif()

# custom make target to strictly test srcSlice test suite
add_custom_target(test_srcslice
    COMMAND ${CMAKE_CTEST_COMMAND} -L srcslice # only run ctest testcases with the srcslice label attached
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS build_srcslice_tests # ensure srcslice tests are built
)

# Download the CLI11 parser into directory named "external".
set(CMAKE_EXTERNAL_SOURCE_DIR ${CMAKE_SOURCE_DIR}/external) # Creates folder if doesn't exists.
# Name of file is needed since content of file itself are being downloaded.
file(DOWNLOAD https://github.com/CLIUtils/CLI11/releases/download/v2.3.2/CLI11.hpp ${CMAKE_EXTERNAL_SOURCE_DIR}/CLI11.hpp)
