find_package(SDL2 REQUIRED)

add_executable(cppu_frontend
    main.cpp
)

target_link_libraries(cppu_frontend PRIVATE cppu::cppu SDL2::SDL2)

if(MSVC)
    target_compile_options(cppu_frontend PRIVATE /W4)
else()
    target_compile_options(cppu_frontend PRIVATE -Wall -Wextra -Wpedantic)
endif()
