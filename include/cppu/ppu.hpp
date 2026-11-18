#pragma once

#include <array>
#include <cstdint>

namespace cppu {

enum class Reg : std::uint8_t {
    LCDC = 0x0,
    STAT = 0x1,
    SCY  = 0x2,
    SCX  = 0x3,
    LY   = 0x4,
    LYC  = 0x5,
    DMA  = 0x6,
    BGP  = 0x7,
    OBP0 = 0x8,
    OBP1 = 0x9,
    WY   = 0xA,
    WX   = 0xB
};

namespace lcdc {
    inline constexpr std::uint8_t kEnable        = 0x80; // LCD and PPU on
    inline constexpr std::uint8_t kWndTileMap    = 0x40; // 0: VRAM+0x1800, 1: VRAM+0x1C00
    inline constexpr std::uint8_t kWndEnable     = 0x20;
    inline constexpr std::uint8_t kBgWndTileData = 0x10; // 0: VRAM+0x0800 signed, 1: VRAM+0x0000 unsigned
    inline constexpr std::uint8_t kBgTileMap     = 0x08; // 0: VRAM+0x1800, 1: VRAM+0x1C00
    inline constexpr std::uint8_t kObjSize       = 0x04; // 0: 8x8, 1: 8x16
    inline constexpr std::uint8_t kObjEnable     = 0x02;
    inline constexpr std::uint8_t kBgWndEnable   = 0x01;
}

namespace stat {
    inline constexpr std::uint8_t kLycIntSelect      = 0x40;
    inline constexpr std::uint8_t kMode2IntSelect    = 0x20;
    inline constexpr std::uint8_t kMode1IntSelect    = 0x10;
    inline constexpr std::uint8_t kMode0IntSelect    = 0x08;
    inline constexpr std::uint8_t kLycEqualsLy       = 0x04; // read-only
    inline constexpr std::uint8_t kModeMask          = 0x03; // read-only (2-bits)
}

inline constexpr int kScreenWidth  = 160;
inline constexpr int kScreenHeight = 144;

// Master clock: 4.194304 MHz. On the DMG the PPU advances one "dot" per
// master clock cycle; one frame is 154 lines * 456 dots = 70224 dots.
inline constexpr int kDotsPerLine   = 456;
inline constexpr int kLinesPerFrame = 154;
inline constexpr int kDotsPerFrame  = kDotsPerLine * kLinesPerFrame;

// Mode 2 (OAM scan) always lasts 80 dots. Mode 3 (drawing) lasts 172-289
// dots depending on SCX, the window and objects; HBlank pads out the rest.
inline constexpr int kOamScanDots = 80;

inline constexpr std::uint8_t kIntVBlank = 0x01;
inline constexpr std::uint8_t kIntStat   = 0x02;

class PPU {
public:
    PPU() = default;

    // Advance the PPU by one master clock cycle (one dot).
    void tick();

    // VRAM access
    std::uint8_t read_vram(std::uint16_t addr) const;
    void write_vram(std::uint16_t addr, std::uint8_t value);

    // OAM access
    std::uint8_t read_oam(std::uint8_t addr) const;
    void write_oam(std::uint8_t addr, std::uint8_t value);

    // Register access
    std::uint8_t read_register(Reg reg) const;
    void write_register(Reg reg, std::uint8_t value);

    // Interrupts raised since the last call (kIntVBlank | kIntStat).
    // The host ORs the result into its IF register.
    uint8_t take_interrupts();

    // Output image: one byte per pixel, holding a shade 0-3
    // (0 = lightest, 3 = darkest). Row-major, kScreenWidth * kScreenHeight.
    const std::uint8_t* framebuffer() const { return framebuffer_.data(); }

    // Returns true once per frame (at the start of VBlank)
    bool frame_ready();

    bool lcdc_enable() const;
    bool lcdc_wnd_enable() const;
    bool lcdc_tile_map_area() const;
    bool lcdc_bg_wnd_tile_data_area() const;
    bool lcdc_bg_tile_map_area() const;
    bool lcdc_obj_size() const;
    bool lcdc_obj_enable() const;
    bool lcdc_bg_wnd_enable() const;

    bool stat_int_mode_0() const;
    bool stat_int_mode_1() const;
    bool stat_int_mode_2() const;
    bool stat_int_mode_lyc() const;
    bool stat_lyc() const;
    uint8_t stat_ppu_mode() const;

private:
    // Values of STAT bits 1-0.
    enum class Mode : std::uint8_t {
        HBlank  = 0,
        VBlank  = 1,
        OamScan = 2,
        Drawing = 3,
    };

    Mode mode() const { return static_cast<Mode>(stat_ & stat::kModeMask); }
    void set_mode(Mode mode);

    // Per-dot work for each mode.
    void tick_oam_scan();
    void tick_drawing();

    // Called after the last dot of a line: advances LY, picks the next mode.
    void end_line();

    std::array<std::uint8_t, kScreenWidth * kScreenHeight> framebuffer_{};

    // VRAM
    std::array<std::uint8_t, 0x2000> vram_{};

    // OAM (Object Attribute Memory)
    std::array<std::uint8_t, 0xA0> oam_{};

    // LCDC register (0xFF40)
    std::uint8_t lcdc_ = 0;

    // LY and LYC registers (0xFF44, 0xFF45)
    std::uint8_t ly_ = 0;
    std::uint8_t lyc_ = 0;
    
    // STAT register (0xFF41). Starts in mode 2 so line 0 begins with OAM scan.
    std::uint8_t stat_ = static_cast<std::uint8_t>(Mode::OamScan);

    // Scroll registers (0xFF42, 0xFF43)
    std::uint8_t scy_ = 0;
    std::uint8_t scx_ = 0;

    // BG palette register (0xFF47)
    std::uint8_t bgp_ = 0;

    // OBJ palette registers (0xFF48, 0xFF49)
    std::uint8_t obp0_ = 0;
    std::uint8_t obp1_ = 0;

    // Window position registers (0xFF4A, 0xFF4B)
    std::uint8_t wx_ = 0;
    std::uint8_t wy_ = 0;

    // Dots since the start of the current line (0-455)
    std::uint16_t dots_ = 0;
    
    // X position of the pixel being drawn in mode 3
    std::uint8_t mode3_x_ = 0;
};

} // namespace cppu
