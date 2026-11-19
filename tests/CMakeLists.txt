add_executable(cppu_bg_render_test
    bg_render_test.cpp
)

target_link_libraries(cppu_bg_render_test PRIVATE cppu::cppu)

if(MSVC)
    target_compile_options(cppu_bg_render_test PRIVATE /W4)
else()
    target_compile_options(cppu_bg_render_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME bg_render COMMAND cppu_bg_render_test)
