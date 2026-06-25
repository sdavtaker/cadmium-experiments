cmake_minimum_required(VERSION 3.28)

# Toolchain must be set before project() so CMake loads vcpkg during compiler detection.
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "Path to vcpkg toolchain file")
    if(NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
        message(FATAL_ERROR "VCPKG_ROOT is set to '$ENV{VCPKG_ROOT}' but toolchain file not found: ${CMAKE_TOOLCHAIN_FILE}")
    endif()
endif()

project(cadmium-experiments VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(cadmium CONFIG REQUIRED)
find_package(cdcommons CONFIG REQUIRED)

# Testing
find_package(Catch2 3 CONFIG REQUIRED)
include(CTest)
enable_testing()

# Helper: build one executable per TIME variant.
# Usage: cadmium_add_time_variants(<name-prefix> <source> [LINK <libs...>])
function(cadmium_add_time_variants prefix source)
    cmake_parse_arguments(ARG "" "" "LINK" ${ARGN})
    foreach(variant IN ITEMS float double rational decimal rsfp mbfp)
        set(target "${prefix}-${variant}")
        string(TOUPPER "${variant}" variant_upper)
        add_executable(${target} ${source})
        target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}")
        target_compile_definitions(${target} PRIVATE "CADMIUM_TIME_${variant_upper}")
        target_compile_options(${target} PRIVATE -Wall -Wextra -pedantic)
        target_link_libraries(${target} PRIVATE cadmium::cadmium ${ARG_LINK})
        if(NOT (variant STREQUAL "float" OR variant STREQUAL "double" OR variant STREQUAL "rational"))
            target_link_libraries(${target} PRIVATE cdcommons::cdcommons)
        endif()
    endforeach()
endfunction()

add_subdirectory(vdw14-pdevs)
add_subdirectory(vdw14-devs)
add_subdirectory(ckw10-lbm)
add_subdirectory(ckw10-ncs)
add_subdirectory(qss1-ball-downstairs)

# Code Formatting
find_program(CLANG_FORMAT
    NAMES clang-format clang-format-18 clang-format-17 clang-format-16 clang-format-15
    DOC "Path to clang-format executable"
)

if(CLANG_FORMAT)
    file(GLOB_RECURSE EXPERIMENT_FORMAT_FILES
        CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/vdw14-pdevs/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/vdw14-pdevs/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/vdw14-devs/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/vdw14-devs/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/ckw10-lbm/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/ckw10-lbm/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/ckw10-ncs/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/ckw10-ncs/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/qss1-ball-downstairs/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/qss1-ball-downstairs/*.cpp"
    )

    add_custom_target(experiments-format
        COMMAND ${CLANG_FORMAT} -i -style=file ${EXPERIMENT_FORMAT_FILES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Applying clang-format to experiment sources"
        VERBATIM
    )

    add_custom_target(experiments-check-format
        COMMAND ${CLANG_FORMAT} -style=file --dry-run -Werror ${EXPERIMENT_FORMAT_FILES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Checking clang-format compliance"
        VERBATIM
    )
else()
    message(STATUS "clang-format not found; 'format' and 'check-format' targets unavailable")
endif()

# PDF Documentation
find_program(PDFLATEX pdflatex DOC "Path to pdflatex")
if(PDFLATEX)
    set(DOCS_OUTPUT_DIR ${CMAKE_BINARY_DIR}/docs)
    add_custom_target(docs
        COMMAND ${CMAKE_COMMAND} -E make_directory ${DOCS_OUTPUT_DIR}
        COMMAND ${PDFLATEX} -interaction=nonstopmode -output-directory ${DOCS_OUTPUT_DIR} main.tex
        COMMAND ${PDFLATEX} -interaction=nonstopmode -output-directory ${DOCS_OUTPUT_DIR} main.tex
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/docs
        COMMENT "Building PDF documentation -> build/docs/main.pdf"
        VERBATIM
    )
else()
    message(STATUS "pdflatex not found; 'docs' target unavailable")
endif()
