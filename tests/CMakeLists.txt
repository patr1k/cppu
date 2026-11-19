function(cppu_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE cppu::cppu)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

cppu_add_test(cppu_bg_render_test bg_render_test.cpp)
add_test(NAME bg_render COMMAND cppu_bg_render_test)

cppu_add_test(cppu_window_render_test window_render_test.cpp)
add_test(NAME window_render COMMAND cppu_window_render_test)

cppu_add_test(cppu_object_render_test object_render_test.cpp)
add_test(NAME object_render COMMAND cppu_object_render_test)

cppu_add_test(cppu_access_test access_test.cpp)
add_test(NAME access COMMAND cppu_access_test)
