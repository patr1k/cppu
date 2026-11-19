# dmg-acid2 test host: Peanut-GB (vendored, patched) with cppu as its PPU.
# Off by default; enable with -DCPPU_BUILD_ACID2=ON. See README.md.

enable_language(C)
find_package(SDL2 REQUIRED)

# Peanut-GB is a single-header C99 emulator; peanut_host.c holds its
# implementation. It's third-party code, so no extra warnings for it.
add_library(cppu_acid2_peanut STATIC peanut_host.c)
set_target_properties(cppu_acid2_peanut PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
target_include_directories(cppu_acid2_peanut PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(cppu_acid2
    main.cpp
    cppu_bridge.cpp
)
target_link_libraries(cppu_acid2 PRIVATE cppu::cppu cppu_acid2_peanut SDL2::SDL2)

if(MSVC)
    target_compile_options(cppu_acid2 PRIVATE /W4)
else()
    target_compile_options(cppu_acid2 PRIVATE -Wall -Wextra -Wpedantic)
endif()
