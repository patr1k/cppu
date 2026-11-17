# cppu

A standalone emulator for the **Game Boy (DMG) Picture Processing Unit**, written in C++20.

cppu emulates only the PPU, not a whole Game Boy. It builds as a library that you can drop into a full Game Boy emulator or into a custom project that just needs Game Boy-style graphics. The host program owns the memory and drives the clock; cppu turns that memory into pixels.

A small SDL2 frontend is included for viewing the output while developing.

> **Status:** early scaffolding. The public API is a stub, and the PPU itself hasn't been written yet.

## Goals

- **Accurate at the dot level.** `tick()` advances the PPU by one master clock cycle (one "dot"), so the mode timing (OAM scan → drawing → HBlank → VBlank) can be modeled faithfully.
- **Host-owned memory.** The caller passes in a pointer to VRAM, and the PPU never allocates or frees it.
- **No dependencies in the library.** SDL2 is only used by the frontend.
- **Room to grow.** A Game Boy Color mode may be added later behind a flag.

## Layout

```
cppu/
├── CMakeLists.txt       # top-level build: the cppu library and options
├── include/cppu/        # public headers (what library users include)
│   └── ppu.hpp
├── src/                 # library implementation
│   └── ppu.cpp
├── frontend/            # SDL2 test harness (not part of the library)
│   ├── CMakeLists.txt
│   └── main.cpp
└── docs/                # notes and hardware references
```

## Building

Requirements: CMake ≥ 3.21, a C++20 compiler, and SDL2 (frontend only).

```sh
# Debian/Ubuntu: sudo apt install cmake g++ libsdl2-dev
cmake -S . -B build
cmake --build build
./build/frontend/cppu_frontend
```

### CMake options

| Option                    | Default                 | Description                          |
|---------------------------|-------------------------|--------------------------------------|
| `CPPU_BUILD_FRONTEND`     | `ON` when top-level     | Build the SDL2 frontend              |
| `CPPU_WARNINGS_AS_ERRORS` | `OFF`                   | Treat compiler warnings as errors    |

## Using cppu in another project

```cmake
add_subdirectory(external/cppu)   # frontend is off automatically when cppu is a subproject
target_link_libraries(my_emulator PRIVATE cppu::cppu)
```

```cpp
#include <cppu/ppu.hpp>

std::array<std::uint8_t, 0x2000> vram{};   // 0x8000–0x9FFF
cppu::PPU ppu(vram.data());

// Call once per master clock cycle (4.194304 MHz).
ppu.tick();

// One byte per pixel, holding a shade from 0 to 3, size 160×144.
const std::uint8_t* pixels = ppu.framebuffer();
```

The API is expected to change as the PPU takes shape. Open questions include how the host exposes OAM and the LCD registers (LCDC, STAT, SCY/SCX, LY/LYC, BGP/OBP0/OBP1, WY/WX) and how the PPU signals VBlank and STAT interrupts back to the CPU.

## Timing reference (DMG)

| Quantity          | Value                         |
|-------------------|-------------------------------|
| Master clock      | 4,194,304 Hz                  |
| Dots per scanline | 456                           |
| Scanlines / frame | 154 (144 visible + 10 VBlank) |
| Dots per frame    | 70,224 (~59.73 Hz)            |
| Mode 2 (OAM scan) | 80 dots                       |
| Mode 3 (drawing)  | 172–289 dots (variable)       |
| Mode 0 (HBlank)   | the rest of the 456 dots      |
| Mode 1 (VBlank)   | lines 144–153                 |

## References

- [Pan Docs](https://gbdev.io/pandocs/): the standard Game Boy technical reference
- [The Ultimate Game Boy Talk (33c3)](https://www.youtube.com/watch?v=HyzD8pNlpwI): a great overview of the PPU's FIFO
- [GBEDG: PPU](https://hacktix.github.io/GBEDG/ppu/): an emulator developer's guide to the PPU
- [dmg-acid2](https://github.com/mattcurrie/dmg-acid2): a PPU accuracy test ROM
