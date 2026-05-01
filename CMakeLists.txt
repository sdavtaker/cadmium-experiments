cmake_minimum_required(VERSION 3.28)
project(cadmium-experiments VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CADMIUM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../simulators/cadmium"
    CACHE PATH "Path to cadmium source")

if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "")
endif()

set(CADMIUM_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(CADMIUM_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${CADMIUM_DIR}" cadmium-build)

add_subdirectory(vdw14)
add_subdirectory(vdw14-devs)
