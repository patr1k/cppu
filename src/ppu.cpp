#include <utility>
#include "cppu/ppu.hpp"

namespace cppu {

void PPU::tick() {
    if (!lcdc_enable()) {
        return;
    }

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
        frame_ready_ = false;
    } else if (ly_ == kScreenHeight) {
        set_mode(Mode::VBlank);          // lines 144-153
        frame_ready_ = true;
        pending_interrupts_ |= kIntVBlank;
    }

    update_stat_line();
}

void PPU::set_mode(Mode mode) {
    stat_ = static_cast<std::uint8_t>((stat_ & ~stat::kModeMask) | static_cast<std::uint8_t>(mode));

    switch (mode) {
        case Mode::OamScan:
            // TODO: clear the per-line object list
            break;
        case Mode::Drawing:
            mode3_x_ = 0;
            bg_fifo_.clear();
            obj_fifo_.clear();
            fetch_step_ = PixelFetchStep::GetTile;
            fetch_dots_ = 0;
            fetch_x_ = 0;
            tile_addr_ = 0;
            tile_lo_ = 0;
            tile_hi_ = 0;
            discard_ = scx_ % 8;
            first_fetch_ = true;
            break;
        case Mode::HBlank:
        case Mode::VBlank:
            break;
    }

    update_stat_line();
}

void PPU::tick_oam_scan() {
    // TODO: every 2 dots, check one OAM entry (40 total) against LY and
    // collect up to 10 objects for this line.
}

void PPU::tick_drawing() {
    // FIFO INPUT SIDE
    switch (fetch_step_) {
        case PixelFetchStep::GetTile: {
            if (fetch_dots_ == 1) {
                std::uint16_t base_addr = lcdc_bg_tile_map_area() ? 0x1C00 : 0x1800;
                std::uint8_t row = ((ly_ + scy_) & 0xFF) / 8;
                std::uint8_t col = ((scx_ / 8) + fetch_x_) & 0x1F;
                std::uint16_t addr = static_cast<std::uint16_t>(base_addr + row * 32 + col);
                std::uint8_t tile_idx = read_vram(addr);
                std::uint8_t row_offset = ((ly_ + scy_) & 7) * 2;
                if (lcdc_bg_wnd_tile_data_area()) {
                    // 0x0000-0x0FFF, unsigned tile index
                    tile_addr_ = static_cast<std::uint16_t>(tile_idx * 16 + row_offset);
                } else {
                    // 0x0800-0x17FF, signed tile index
                    std::int8_t idx = static_cast<std::int8_t>(tile_idx);
                    tile_addr_ = static_cast<std::uint16_t>(0x1000 + idx * 16 + row_offset);
                }
            }
            ++fetch_dots_;
            break;
        }
        case PixelFetchStep::GetTileDataLow:
            if (fetch_dots_ == 1) {
                tile_lo_ = read_vram(tile_addr_);
            }
            ++fetch_dots_;
            break;
        case PixelFetchStep::GetTileDataHigh:
            if (fetch_dots_ == 1) {
                tile_hi_ = read_vram(tile_addr_ + 1);
            }
            ++fetch_dots_;
            break;
        case PixelFetchStep::Push: {
            if (bg_fifo_.empty()) {
                // If the background FIFO is empty, we need to fill it with pixels
                for (std::uint8_t i = 0; i < 8; ++i) {
                    std::uint8_t b = 7 - i;
                    std::uint8_t color = (((tile_hi_ >> b) & 1) << 1) | ((tile_lo_ >> b) & 1);
                    bg_fifo_.push(Pixel{color, 0, false}); // placeholder pixel
                }
                ++fetch_x_;
                fetch_step_ = PixelFetchStep::GetTile;
                fetch_dots_ = 0;
            }
            break;
        }
    }

    if (fetch_dots_ == 2 && fetch_step_ != PixelFetchStep::Push) {
        fetch_dots_ = 0;
        if (fetch_step_ == PixelFetchStep::GetTileDataHigh && first_fetch_) {
            // The first tile fetch of each line is done twice; throw away the first one's result
            first_fetch_ = false;
            fetch_step_ = PixelFetchStep::GetTile;
        } else {
            fetch_step_ = static_cast<PixelFetchStep>(static_cast<std::uint8_t>(fetch_step_) + 1);
        }
    }

    // FIFO OUTPUT SIDE
    if (bg_fifo_.size() > 0) {
        auto pixel = bg_fifo_.pop();

        // The first fetch of the line is special: we discard pixels until
        // the SCX offset is satisfied.
        if (discard_ > 0) {
            --discard_;
            return;
        }

        if (!lcdc_bg_wnd_enable()) {
            // If the background/window is disabled, we draw white (color 0)
            framebuffer_[ly_ * kScreenWidth + mode3_x_] = 0;
        } else {
            // Map the pixel color through the BGP register to get the final shade
            framebuffer_[ly_ * kScreenWidth + mode3_x_] = static_cast<std::uint8_t>((bgp_ & (3 << (pixel.color * 2))) >> (pixel.color * 2));
        }
        ++mode3_x_;
    }
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
        case Reg::LCDC: {
            const bool was_on = lcdc_enable();
            lcdc_ = value;
            if (was_on && !lcdc_enable()) {
                ly_ = 0; dots_ = 0;
                set_mode(Mode::HBlank);
            } else if (!was_on && lcdc_enable()) {
                set_mode(Mode::OamScan);
            }
            break;
        }
        case Reg::STAT: {
            std::uint8_t v = stat_ & stat::kModeMask;
            v |= value & (stat::kMode0IntSelect | stat::kMode1IntSelect | stat::kMode2IntSelect | stat::kLycIntSelect);
            stat_ = v;
            update_stat_line();
            break;
        }
        case Reg::SCY:  scy_ = value; break;
        case Reg::SCX:  scx_ = value; break;
        case Reg::LYC: {
            lyc_ = value;
            update_stat_line();
            break;
        }
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

void PPU::update_stat_line() {
    const auto m = mode();
    const bool line =
        (stat_int_mode_0() && m == Mode::HBlank) ||
        (stat_int_mode_1() && m == Mode::VBlank) ||
        (stat_int_mode_2() && m == Mode::OamScan) ||
        (stat_int_mode_lyc() && stat_lyc());
    if (line && !stat_line_) pending_interrupts_ |= kIntStat;
    stat_line_ = line;
}

std::uint8_t PPU::take_interrupts() {
    return std::exchange(pending_interrupts_, 0);
}

bool PPU::frame_ready() {
    return std::exchange(frame_ready_, false);
}

bool PPU::lcdc_enable() const {
    return lcdc_ & lcdc::kEnable;
}

bool PPU::lcdc_tile_map_area() const {
    return lcdc_ & lcdc::kWndTileMap;
}

bool PPU::lcdc_wnd_enable() const {
    return lcdc_ & lcdc::kWndEnable;
}

bool PPU::lcdc_bg_wnd_tile_data_area() const {
    return lcdc_ & lcdc::kBgWndTileData;
}

bool PPU::lcdc_bg_tile_map_area() const {
    return lcdc_ & lcdc::kBgTileMap;
}

bool PPU::lcdc_obj_size() const {
    return lcdc_ & lcdc::kObjSize;
}

bool PPU::lcdc_obj_enable() const {
    return lcdc_ & lcdc::kObjEnable;
}

bool PPU::lcdc_bg_wnd_enable() const {
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
