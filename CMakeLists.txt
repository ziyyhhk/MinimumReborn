cmake_minimum_required(VERSION 3.21)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(MinimumReborn VERSION 1.5.2)

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
    src/config.cpp
    src/rendering.cpp
    src/frame_gate.cpp
    src/benchmark.cpp
    src/system_tuning.cpp
    src/ui.cpp
)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

# Out-of-tree SDK build directory so the SDK install itself is never written to.
add_subdirectory($ENV{GEODE_SDK} ${CMAKE_BINARY_DIR}/geode-sdk)

setup_geode_mod(${PROJECT_NAME})

target_include_directories(${PROJECT_NAME} PRIVATE src)

target_link_libraries(${PROJECT_NAME} geode-sdk)

if (WIN32)
    # timeBeginPeriod / timeEndPeriod (system_tuning.cpp). harmless on non-Windows builds.
    target_link_libraries(${PROJECT_NAME} winmm)
endif()
