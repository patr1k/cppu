#include "cppu/ppu.hpp"

namespace cppu {

void PPU::tick() {
    // TODO: when LCDC bit 7 is clear the PPU is off: LY stays 0, mode 0.

    // 1. Do this dot's work for the current mode.
    switch (mode()) {
        case Mode::OamScan: tick_oam_scan(); break;
        case Mode::Drawing: tick_drawing();  break;
        case Mode::HBlank:  break;           // idle until end of line
        case Mode::VBlank:  break;           // idle until end of frame
    }

    // 2. Advance the clock and handle any mode change it causes.
    ++dots_;

    if (dots_ == kDotsPerLine) {
        end_line();
    } else if (mode() == Mode::OamScan && dots_ == kOamScanDots) {
        set_mode(Mode::Drawing);
    } else if (mode() == Mode::Drawing && mode3_x_ == kScreenWidth) {
        set_mode(Mode::HBlank);
    }
}

void PPU::end_line() {
    dots_ = 0;
    ly_ = static_cast<std::uint8_t>((ly_ + 1) % kLinesPerFrame);

    if (ly_ < kScreenHeight) {
        set_mode(Mode::OamScan);         // next visible line (incl. wrap to 0)
    } else if (ly_ == kScreenHeight) {
        set_mode(Mode::VBlank);          // lines 144-153
        // TODO: raise kIntVBlank, mark frame_ready
    }
    // TODO: LY == LYC coincidence check / STAT interrupt
}

void PPU::set_mode(Mode mode) {
    stat_ = static_cast<std::uint8_t>((stat_ & ~stat::kModeMask) | static_cast<std::uint8_t>(mode));

    switch (mode) {
        case Mode::OamScan:
            // TODO: clear the per-line object list
            break;
        case Mode::Drawing:
            mode3_x_ = 0;
            // TODO: reset fetcher and pixel FIFOs
            break;
        case Mode::HBlank:
        case Mode::VBlank:
            break;
    }
    // TODO: STAT interrupt line (mode 0/1/2 selects) is edge-triggered
}

void PPU::tick_oam_scan() {
    // TODO: every 2 dots, check one OAM entry (40 total) against LY and
    // collect up to 10 objects for this line.
}

void PPU::tick_drawing() {
    // TODO: run the background/window fetcher and push pixels through
    // the FIFOs. Placeholder: emit one pixel per dot.
    ++mode3_x_;
}

std::uint8_t PPU::read_vram(std::uint16_t addr) const {
    if (addr >= vram_.size()) {
        // Invalid VRAM address
        return 0xFF;
    }
    return vram_[addr];
}

void PPU::write_vram(std::uint16_t addr, std::uint8_t value) {
    if (addr >= vram_.size()) {
        // Invalid VRAM address
        return;
    }
    vram_[addr] = value;
}

std::uint8_t PPU::read_oam(std::uint8_t addr) const {
    if (addr >= oam_.size()) {
        // Invalid OAM address
        return 0xFF;
    }
    return oam_[addr];
}

void PPU::write_oam(std::uint8_t addr, std::uint8_t value) {
    if (addr >= oam_.size()) {
        // Invalid OAM address
        return;
    }
    oam_[addr] = value;
}

std::uint8_t PPU::read_register(Reg reg) const {
    switch (reg) {
        case Reg::LCDC: return lcdc_;
        case Reg::STAT: {
            std::uint8_t v = stat_ & ~stat::kLycEqualsLy;
            if (stat_lyc()) v |= stat::kLycEqualsLy;
            return (v | 0x80);
        }
        case Reg::SCY:  return scy_;
        case Reg::SCX:  return scx_;
        case Reg::LY:   return ly_;
        case Reg::LYC:  return lyc_;
        case Reg::BGP:  return bgp_;
        case Reg::OBP0: return obp0_;
        case Reg::OBP1: return obp1_;
        case Reg::WY:   return wy_;
        case Reg::WX:   return wx_;
        default:
            // Invalid register address
            return 0xFF;
    }
}

void PPU::write_register(Reg reg, std::uint8_t value) {
    switch (reg) {
        case Reg::LCDC: lcdc_ = value; break;
        case Reg::STAT: {
            std::uint8_t v = stat_ & stat::kModeMask;
            v |= value & (stat::kMode0IntSelect | stat::kMode1IntSelect | stat::kMode2IntSelect | stat::kLycIntSelect);
            stat_ = v;
            break;
        }
        case Reg::SCY:  scy_ = value; break;
        case Reg::SCX:  scx_ = value; break;
        case Reg::LYC:  lyc_ = value; break;
        case Reg::BGP:  bgp_ = value; break;
        case Reg::OBP0: obp0_ = value; break;
        case Reg::OBP1: obp1_ = value; break;
        case Reg::WY:   wy_ = value; break;
        case Reg::WX:   wx_ = value; break;
        default:
            // Invalid register address, ignore write
            break;
    }
}

bool PPU::lcdc_enable() const {
    // Bit 7 of LCDC (0xFF40) is the LCD enable flag.
    // When this bit is 0, the PPU is disabled and the screen is blank.
    return lcdc_ & lcdc::kEnable;
}

bool PPU::lcdc_tile_map_area() const {
    // Bit 6 of LCDC (0xFF40) is the tile map area flag.
    // 0 = the background tile map is at 0x9800-0x9BFF.
    // 1 = the background tile map is at 0x9C00-0x9FFF.
    return lcdc_ & lcdc::kWndTileMap;
}

bool PPU::lcdc_wnd_enable() const {
    // Bit 5 of LCDC (0xFF40) is the window enable flag.
    // 0 = the window is disabled.
    // 1 = the window is enabled.
    return lcdc_ & lcdc::kWndEnable;
}

bool PPU::lcdc_bg_wnd_tile_data_area() const {
    // Bit 4 of LCDC (0xFF40) is the background/window tile data area flag.
    // 0 = the tile data is at 0x8800-0x97FF (signed addressing).
    // 1 = the tile data is at 0x8000-0x8FFF (unsigned addressing).
    return lcdc_ & lcdc::kBgWndTileData;
}

bool PPU::lcdc_bg_tile_map_area() const {
    // Bit 3 of LCDC (0xFF40) is the background tile map area flag.
    // 0 = the background tile map is at 0x9800-0x9BFF.
    // 1 = the background tile map is at 0x9C00-0x9FFF.
    return lcdc_ & lcdc::kBgTileMap;
}

bool PPU::lcdc_obj_size() const {
    // Bit 2 of LCDC (0xFF40) is the object size flag.
    // 0 = 8x8 sprites.
    // 1 = 8x16 sprites.
    return lcdc_ & lcdc::kObjSize;
}

bool PPU::lcdc_obj_enable() const {
    // Bit 1 of LCDC (0xFF40) is the object enable flag.
    // 0 = sprites are disabled.
    // 1 = sprites are enabled.
    return lcdc_ & lcdc::kObjEnable;
}

bool PPU::lcdc_bg_wnd_enable() const {
    // Bit 0 of LCDC (0xFF40) is the background/window enable flag.
    // 0 = the background and window are disabled.
    // 1 = the background and window are enabled.
    return lcdc_ & lcdc::kBgWndEnable;
}

bool PPU::stat_int_mode_0() const {
    return stat_ & stat::kMode0IntSelect;
}

bool PPU::stat_int_mode_1() const {
    return stat_ & stat::kMode1IntSelect;
}

bool PPU::stat_int_mode_2() const {
    return stat_ & stat::kMode2IntSelect;
}

bool PPU::stat_int_mode_lyc() const {
    return stat_ & stat::kLycIntSelect;
}

bool PPU::stat_lyc() const {
    return ly_ == lyc_;
}

std::uint8_t PPU::stat_ppu_mode() const {
    return static_cast<std::uint8_t>(stat_ & stat::kModeMask);
}

} // namespace cppu
