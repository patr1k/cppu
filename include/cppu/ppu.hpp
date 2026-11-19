#pragma once

#include <array>
#include <cstdint>

#include "cppu/fifo.hpp"

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

namespace oam {
    inline constexpr std::uint8_t kBgPriority = 0x80;
    inline constexpr std::uint8_t kYFlip = 0x40;
    inline constexpr std::uint8_t kXFlip = 0x20;
    inline constexpr std::uint8_t kPalette = 0x10;
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
inline constexpr int kMaxObjectsPerLine = 10;

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
    std::uint8_t take_interrupts();

    // Output image: one byte per pixel, holding a shade 0-3
    // (0 = lightest, 3 = darkest). Row-major, kScreenWidth * kScreenHeight.
    const std::uint8_t* framebuffer() const { return framebuffer_.data(); }

    // Returns true once per frame (at the start of VBlank)
    bool frame_ready();

    bool lcdc_enable() const;
    bool lcdc_wnd_enable() const;
    bool lcdc_wnd_tile_map_area() const;
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
    std::uint8_t stat_ppu_mode() const;

private:
    // Values of STAT bits 1-0.
    enum class Mode : std::uint8_t {
        HBlank  = 0,
        VBlank  = 1,
        OamScan = 2,
        Drawing = 3,
    };

    // Steps of the BG/window tile fetcher. The first three take 2 dots each;
    // Push waits until the BG FIFO is empty.
    enum class BgFetchStep : std::uint8_t {
        GetTile,
        GetTileDataLow,
        GetTileDataHigh,
        Push
    };

    // An object selected by OAM scan: a copy of its 4 OAM bytes, unmodified.
    struct Object {
        std::uint8_t y;     // screen Y + kObjYOffset
        std::uint8_t x;     // screen X + kObjXOffset
        std::uint8_t tile;
        std::uint8_t attrs; // oam::k* bits
    };

    // OAM positions are offset so objects can sit partly off the top/left.
    static constexpr int kObjYOffset = 16;
    static constexpr int kObjXOffset = 8;

    // Tile map offsets in VRAM, selected by LCDC.3 (BG) and LCDC.6 (window).
    static constexpr std::uint16_t kTileMap0 = 0x1800;
    static constexpr std::uint16_t kTileMap1 = 0x1C00;

    // --- Mode and line control ---------------------------------------------
    Mode mode() const { return static_cast<Mode>(stat_ & stat::kModeMask); }
    void set_mode(Mode mode);

    // Called after the last dot of a line: advances LY, picks the next mode.
    void end_line();

    void update_stat_line();

    // --- Mode 2: OAM scan ----------------------------------------------------
    void tick_oam_scan();
    int obj_height() const { return lcdc_obj_size() ? 16 : 8; }

    // --- Mode 3: drawing (tick_drawing runs the steps below each dot) --------
    void tick_drawing();

    // Object fetch: stalls the BG fetcher and output for 6-11 dots.
    void tick_object_fetch();
    bool try_start_object_fetch();
    std::uint8_t obj_fetch_length(const Object& obj);
    void merge_object_row(const Object& obj);

    // Window: restarts the BG fetcher on the window's tile map.
    bool window_starts_here() const;
    void start_window();

    // BG/window fetcher: fills the BG FIFO one tile row at a time.
    void tick_bg_fetcher();
    std::uint16_t bg_tile_row_addr() const;
    void push_bg_row();

    // Pixel output: pops one pixel per dot and writes it to the framebuffer.
    void output_pixel();
    std::uint8_t mix_pixel(const Pixel& bg, const Pixel& obj) const;
    std::uint8_t apply_palette(std::uint8_t palette, std::uint8_t color) const;

    // True if a pixel will be output on this dot: the BG FIFO has one, or
    // the fetcher is about to push a tile row into it.
    bool pixel_expected() const;

    // --- State ---------------------------------------------------------------
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

    // Pending interrupts (kIntVBlank | kIntStat)
    std::uint8_t pending_interrupts_ = 0;

    // STAT interrupt line state (true = high, false = low). Edge-triggered.
    bool stat_line_ = false;

    // True once per frame (at the start of VBlank)
    bool frame_ready_ = false;

    // --- Mode 3 pixel FIFOs. Slot i of each is screen pixel mode3_x_ + i.
    PixelFifo bg_fifo_{};
    PixelFifo obj_fifo_{};

    // --- BG/window fetcher (reset at the start of each line's mode 3) ---
    BgFetchStep bg_fetch_step_ = BgFetchStep::GetTile;
    std::uint8_t bg_fetch_dots_ = 0;      // dots spent in the current step (0-1)
    std::uint8_t bg_fetch_tile_x_ = 0;    // tile column being fetched, counted from the line start
    std::uint16_t bg_tile_addr_ = 0;      // VRAM offset of the tile row being fetched
    std::uint8_t bg_tile_lo_ = 0;         // its low bitplane byte
    std::uint8_t bg_tile_hi_ = 0;         // its high bitplane byte
    bool dummy_fetch_pending_ = false;    // the first fetch of each line is thrown away
    std::uint8_t scx_discard_ = 0;        // BG pixels still to drop for fine scroll (SCX % 8)

    // --- Window ---
    bool wy_latched_ = false;             // LY has matched WY earlier this frame
    bool in_wnd_ = false;                 // the fetcher is drawing the window on this line
    std::uint8_t wnd_line_ = 0;           // window's own line counter (only counts lines it was drawn on)

    // --- Objects selected by OAM scan for the current line, in OAM order ---
    std::array<Object, kMaxObjectsPerLine> line_objs_{};
    std::uint8_t line_obj_count_ = 0;

    // --- Object fetch ---
    std::int8_t obj_fetch_idx_ = -1;      // index into line_objs_ being fetched; -1 = none
    std::uint8_t obj_fetch_dots_ = 0;     // dots spent on it so far
    std::uint8_t obj_fetch_len_ = 0;      // its total length (6-11 dots)
    std::uint16_t obj_fetched_mask_ = 0;  // bit i set = line_objs_[i] already fetched

    // The BG/window tile that last charged an object the extra "wait for the
    // BG fetcher" dots, so later objects in the same tile only pay 6.
    // kNoPenaltyTile = none yet this line. (Not -1: an object hanging off the
    // left edge sits in BG tile -1.) See obj_fetch_length().
    static constexpr int kNoPenaltyTile = -1000;
    int obj_penalty_tile_ = kNoPenaltyTile;
};

} // namespace cppu
