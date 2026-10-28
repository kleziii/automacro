cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
endif()

if (APPLE AND NOT IOS AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64")
endif()

project(AutoMacro VERSION 1.3.0)

include($ENV{GEODE_SDK}/cmake/CPM.cmake)

CPMDeclarePackage(nontype_functional
    NAME nontype_functional
    GIT_REPOSITORY "https://github.com/geode-sdk/nontype_functional"
    GIT_TAG 7f190cd793f0d46c19ee1cd2c3d5b50ffd3db4f7
)

add_library(${PROJECT_NAME} SHARED src/main.cpp)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# libGDR - parser formatu GDR2 (ten sam commit, ktory uzywa Click Guide Visualizer)
CPMAddPackage("gh:maxnut/GDReplayFormat#2a0ca3cff9b1d16863037a464da5fe27c28a5823")

if (TARGET GDReplayFormat)
    target_link_libraries(${PROJECT_NAME} GDReplayFormat)
elseif (DEFINED GDReplayFormat_SOURCE_DIR)
    target_include_directories(${PROJECT_NAME} PRIVATE ${GDReplayFormat_SOURCE_DIR}/include)
endif()

setup_geode_mod(${PROJECT_NAME})
