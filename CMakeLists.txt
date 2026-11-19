cmake_minimum_required(VERSION 3.21)

project(cppu
    VERSION 0.1.0
    DESCRIPTION "Cycle-level Game Boy (DMG) PPU emulator library"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
# The SDL2 frontend is only built by default when cppu is the top-level
# project, so embedding cppu in another emulator doesn't pull in SDL2.
option(CPPU_BUILD_FRONTEND "Build the SDL2 test frontend" ${PROJECT_IS_TOP_LEVEL})
option(CPPU_BUILD_TESTS "Build the cppu tests" ${PROJECT_IS_TOP_LEVEL})
option(CPPU_BUILD_ACID2 "Build the dmg-acid2 test host (Peanut-GB + cppu, needs SDL2)" OFF)
option(CPPU_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(cppu
    src/ppu.cpp
)
add_library(cppu::cppu ALIAS cppu)

target_include_directories(cppu
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(cppu PUBLIC cxx_std_20)
set_target_properties(cppu PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(cppu PRIVATE /W4 $<$<BOOL:${CPPU_WARNINGS_AS_ERRORS}>:/WX>)
else()
    target_compile_options(cppu PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        $<$<BOOL:${CPPU_WARNINGS_AS_ERRORS}>:-Werror>)
endif()

# ---------------------------------------------------------------------------
# Frontend
# ---------------------------------------------------------------------------
if(CPPU_BUILD_FRONTEND)
    add_subdirectory(frontend)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(CPPU_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
if(CPPU_BUILD_ACID2)
    add_subdirectory(tools/acid2)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS cppu EXPORT cppuTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT cppuTargets NAMESPACE cppu:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppu)
