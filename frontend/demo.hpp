#pragma once

// Background + window demo scene for the cppu frontend.
//
// Loads a few tiles, a scrolling background map and a window "status bar"
// into the PPU, then updates the scroll and window registers once per frame.
// It only talks to the PPU through its public interface (VRAM writes and
// registers), the same way a game would.

#include <cstdint>

#include "cppu/ppu.hpp"

namespace demo {

// One frame's worth of user input.
struct Input {
    int  scroll_x = 0;          // -1, 0 or +1 (arrow keys)
    int  scroll_y = 0;
    bool toggle_auto_scroll = false;
    bool toggle_window = false;
};

class Demo {
public:
    // Loads tiles and maps, sets the registers and turns the LCD on.
    explicit Demo(cppu::PPU& ppu);

    // Call once per frame, before ticking the PPU through the frame.
    void update(const Input& input);

private:
    void write_status_text();

    cppu::PPU& ppu_;
    int    frame_ = 0;
    double pos_x_ = 0.0;
    double pos_y_ = 0.0;
    bool   auto_scroll_ = true;
    bool   window_shown_ = true;
    int    wy_ = 0;
};

} // namespace demo
