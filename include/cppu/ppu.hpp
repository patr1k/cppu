#pragma once

#include <array>
#include <cstdint>

namespace cppu {

inline constexpr int kScreenWidth  = 160;
inline constexpr int kScreenHeight = 144;

// Master clock: 4.194304 MHz. On the DMG the PPU advances one "dot" per
// master clock cycle; one frame is 154 lines * 456 dots = 70224 dots.
inline constexpr int kDotsPerLine   = 456;
inline constexpr int kLinesPerFrame = 154;
inline constexpr int kDotsPerFrame  = kDotsPerLine * kLinesPerFrame;

class PPU {
public:
    // `vram` points to memory owned by the caller. The PPU never frees it.
    explicit PPU(std::uint8_t* vram);

    // Advance the PPU by one master clock cycle (one dot).
    void tick();

    // Output image: one byte per pixel, holding a shade 0-3
    // (0 = lightest, 3 = darkest). Row-major, kScreenWidth * kScreenHeight.
    const std::uint8_t* framebuffer() const { return framebuffer_.data(); }

private:
    std::uint8_t* vram_;
    std::array<std::uint8_t, kScreenWidth * kScreenHeight> framebuffer_{};

    // TODO: mode state machine, LY/dot counters, registers, OAM, FIFOs...
};

} // namespace cppu
