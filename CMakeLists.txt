cmake_minimum_required(VERSION 3.21)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(PerGamemodeGlow VERSION 1.1.0)

# Geometry Dash 2.2081 is the only supported target. The mod uses the native
# Geode 5.11.2 loader and its bindings; no compatibility shims are needed.
set(GEODE_GD_VERSION "2.2081" CACHE STRING "Geometry Dash version")
set(GEODE_COMP_GD_VERSION 22081 CACHE STRING "Geode compatibility version")

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define the GEODE_SDK environment variable to point to the Geode SDK.")
else()
    message(STATUS "Found Geode SDK: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# The generated bindings reference types from headers that are not included by
# their own base headers. The SDK adds <Geode/Bindings.hpp> as an INTERFACE PCH,
# which reaches this mod but not GeodeBindings' own generated sources. Force the
# header into GeodeBindings as well.
if (TARGET GeodeBindings)
    if (MSVC)
        target_compile_options(GeodeBindings PRIVATE "/FIGeode/Bindings.hpp")
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(GeodeBindings PRIVATE "-include" "Geode/Bindings.hpp")
    endif()
endif()

setup_geode_mod(${PROJECT_NAME})
