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

    if (in_wnd_) ++wnd_line_;

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
            line_obj_count_ = 0;
            if (ly_ == 0) {
                wy_triggered_ = false;
                wnd_line_ = 0;
            }
            if (ly_ == wy_) {
                wy_triggered_ = true;
            }
            in_wnd_ = false;
            break;

        case Mode::Drawing:
            // Set the X coord we're drawing
            mode3_x_ = 0;

            // Clear the FIFOs
            bg_fifo_.clear();
            obj_fifo_.clear();

            // Set up the fetcher state
            fetch_step_ = PixelFetchStep::GetTile;
            fetch_dots_ = 0;
            fetch_x_ = 0;
            tile_addr_ = 0;
            tile_lo_ = 0;
            tile_hi_ = 0;
            discard_ = scx_ % 8;
            first_fetch_ = true;

            obj_fetched_ = 0;
            obj_fetch_idx_ = -1;

            obj_penalty_tile_ = kNoPenaltyTile;
            break;

        case Mode::HBlank:
        case Mode::VBlank:
            break;
    }

    update_stat_line();
}

void PPU::tick_oam_scan() {
    std::uint16_t idx = dots_ / 2;
    std::uint8_t height = lcdc_obj_size() ? 16 : 8;

    if (dots_ & 1) {
        std::uint8_t y = oam_[idx * 4];
        std::uint16_t line = static_cast<std::uint16_t>(ly_ + 16);
        std::uint16_t y_height = static_cast<std::uint16_t>(y + height);
        if (
            line >= y 
            && line < y_height 
            && line_obj_count_ < kMaxObjectsPerLine
        ) {
            std::uint8_t x = oam_[idx * 4 + 1];
            std::uint8_t tile = oam_[idx * 4 + 2];
            std::uint8_t attrs = oam_[idx * 4 + 3];
            line_obj_list_[line_obj_count_] = Object{y, x, tile, attrs};
            ++line_obj_count_;
        }
    }
}

bool PPU::pixel_expected() const {
    return (!bg_fifo_.empty() || fetch_step_ == PixelFetchStep::Push);
}

std::uint8_t PPU::apply_palette(std::uint8_t palette, std::uint8_t color) const {
    return (palette >> (color * 2)) & 3;
}

// How many dots the fetch of `obj` stalls mode 3: 6 to 11.
//
// On hardware, the object fetch can't start until the BG fetcher has
// finished fetching the tile it's working on, and then the object fetch
// itself takes 6 dots. Pan Docs describes the cost in terms of the BG tile
// that contains the object's leftmost pixel:
//
//   - If an earlier object on this line already paid the wait for that
//     tile, this one only pays 6.
//   - Otherwise it pays 6 + max(0, 5 - p), where p (0-7) is the position
//     of the object's leftmost pixel within that tile. 5 - p is "pixels of
//     the tile strictly to the right of it, minus 2". So an object at the
//     start of a tile costs 11, and one at p >= 5 costs 6.
//   - An object at X = 0 always costs 11, whatever SCX is.
//
// Our fetcher's timing within a tile doesn't line up with the hardware's
// (we push and pop in the same dot), so simulating the wait would give the
// wrong numbers. Instead we compute the documented cost directly.
//
// Less certain: objects at X = 1-7 (partly off the left edge) and objects
// whose leftmost pixel lands exactly where the window starts. Check these
// against test ROMs (e.g. mealybug-tearoom) once a CPU is attached.
std::uint8_t PPU::obj_fetch_length(const Object& obj) {
    constexpr int kObjFetchDots = 6;

    if (obj.x == 0) {
        return 11;
    }

    // Screen X of the object's leftmost pixel. Negative (-7..-1) for
    // objects hanging off the left edge (X = 1-7).
    const int sx = obj.x - 8;

    // Find the tile under that pixel: its position p within the tile, and
    // an ID for the tile so we can tell whether it has already been charged.
    // `int` math throughout: `& 7` and `>> 3` on negative values round
    // toward -infinity in C++20, which is what we want here.
    int p;
    int tile_id;
    const int wnd_start = wx_ - 7;
    if (in_wnd_ && sx >= wnd_start) {
        // Window tiles are aligned to where the window starts. Offset their
        // IDs so they can't collide with BG tile IDs.
        p = (sx - wnd_start) & 7;
        tile_id = 64 + ((sx - wnd_start) >> 3);
    } else {
        // BG tiles are aligned to the scrolled background: screen pixel sx
        // is BG pixel sx + SCX.
        p = (sx + scx_) & 7;
        tile_id = (sx + (scx_ & 7)) >> 3;
    }

    if (tile_id == obj_penalty_tile_) {
        return kObjFetchDots;
    }
    obj_penalty_tile_ = tile_id;
    const int wait = p < 5 ? 5 - p : 0;
    return static_cast<std::uint8_t>(kObjFetchDots + wait);
}

void PPU::tick_drawing() {
    if (obj_fetch_idx_ >= 0) {
        // An object fetch is in progress. The BG fetcher and pixel output stay
        // frozen until it finishes, which is what makes mode 3 longer.
        // obj_fetch_len_ (6-11 dots) was set when the fetch started.
        ++obj_fetch_dots_;
        if (obj_fetch_dots_ < obj_fetch_len_) return;

        // Last dot of the fetch: read the tile row and merge it into the
        // object FIFO. (Doing all the VRAM reads on one dot only matters if
        // the CPU writes VRAM during the fetch.)
        auto obj = line_obj_list_[obj_fetch_idx_];
        std::uint8_t height = lcdc_obj_size() ? 16 : 8;
        std::uint8_t row = static_cast<std::uint8_t>(ly_ + 16 - obj.y);
        if (obj.attrs & oam::kYFlip) row = static_cast<std::uint8_t>(height - 1 - row);
        std::uint8_t tile = height == 16 ? (obj.tile & 0xFE) : obj.tile;
        std::uint16_t addr = static_cast<std::uint16_t>(tile * 16 + row * 2);
        std::uint8_t lo = vram_[addr + 0];
        std::uint8_t hi = vram_[addr + 1];
        
        while (obj_fifo_.size() < 8) obj_fifo_.push(Pixel{});

        std::uint8_t skip = static_cast<std::uint8_t>(mode3_x_ + 8 - obj.x);
        for (std::uint8_t j = skip; j < 8; j++) {
            std::uint8_t bit = (obj.attrs & oam::kXFlip) ? j : 7 - j;
            std::uint8_t color = ((hi >> bit) & 1) << 1 | ((lo >> bit) & 1);
            if (obj_fifo_.at(j - skip).color == 0 && color != 0) {
                obj_fifo_.at(j - skip) = Pixel{
                    color, 
                    (obj.attrs & oam::kPalette) != 0, 
                    (obj.attrs & oam::kBgPriority) != 0
                };
            }
        }
        obj_fetched_ |= 1 << obj_fetch_idx_;
        obj_fetch_idx_ = -1;
        return;
    }

    if (
        !in_wnd_ 
        && lcdc_wnd_enable() 
        && wy_triggered_ 
        && static_cast<std::uint16_t>(mode3_x_+7) == wx_ 
        && pixel_expected()
    ) {
        // drawing a window
        in_wnd_ = true;
        bg_fifo_.clear();
        fetch_step_ = PixelFetchStep::GetTile;
        fetch_dots_ = 0;
        fetch_x_ = 0;
        discard_ = 0;
    }

    if (lcdc_obj_enable() && discard_ == 0 && pixel_expected()) {
        for (std::uint8_t i = 0; i < line_obj_count_; i++) {
            if (((obj_fetched_ >> i) & 1) == 0) {
                if (line_obj_list_[i].x <= mode3_x_ + 8) {
                    // Start fetching this object. This dot counts as the
                    // first dot of the fetch.
                    obj_fetch_idx_ = static_cast<std::int8_t>(i);
                    obj_fetch_dots_ = 1;
                    obj_fetch_len_ = obj_fetch_length(line_obj_list_[i]);
                    return;
                }
            }
        }
    }

    // FIFO INPUT SIDE
    switch (fetch_step_) {

        // Get the tile index from the background tile map, then compute the address of the
        // tile data for the row of pixels corresponding to LY.
        case PixelFetchStep::GetTile: {
            if (fetch_dots_ == 1) {
                std::uint16_t base_addr;
                std::uint8_t row, col, row_offset;
                if (in_wnd_) {
                    base_addr = lcdc_wnd_tile_map_area() ? 0x1C00 : 0x1800;
                    row = wnd_line_ / 8;
                    col = fetch_x_;
                    row_offset = (wnd_line_ & 7) * 2;
                } else {
                    base_addr = lcdc_bg_tile_map_area() ? 0x1C00 : 0x1800;
                    row = ((ly_ + scy_) & 0xFF) / 8;
                    col = ((scx_ / 8) + fetch_x_) & 0x1F;
                    row_offset = ((ly_ + scy_) & 7) * 2;
                }
                std::uint16_t addr = static_cast<std::uint16_t>(base_addr + row * 32 + col);
                std::uint8_t tile_idx = read_vram(addr);
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

        // Fetch the low byte of the tile data for this row of pixels.
        case PixelFetchStep::GetTileDataLow:
            if (fetch_dots_ == 1) {
                tile_lo_ = read_vram(tile_addr_);
            }
            ++fetch_dots_;
            break;

        // Fetch the high byte of the tile data for this row of pixels.
        case PixelFetchStep::GetTileDataHigh:
            if (fetch_dots_ == 1) {
                tile_hi_ = read_vram(tile_addr_ + 1);
            }
            ++fetch_dots_;
            break;
        
        // Push the 8 pixels of this tile row into the background FIFO.
        case PixelFetchStep::Push: {
            if (bg_fifo_.empty()) {
                // If the background FIFO is empty, we need to fill it with pixels
                for (std::uint8_t i = 0; i < 8; ++i) {
                    std::uint8_t b = 7 - i;
                    std::uint8_t color = (((tile_hi_ >> b) & 1) << 1) | ((tile_lo_ >> b) & 1);
                    bg_fifo_.push(Pixel{color, 0, false});
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
        auto bg_pixel = bg_fifo_.pop();

        // Drop the first scx % 8 pixels for fine scroll
        if (discard_ > 0) {
            --discard_;
            return;
        }

        Pixel obj_pixel{};
        if (!obj_fifo_.empty()) {
            obj_pixel = obj_fifo_.pop();
        }

        std::uint8_t shade;
        if (obj_pixel.color == 0) {
            if (!lcdc_bg_wnd_enable()) {
                shade = 0;
            } else {
                shade = apply_palette(bgp_, bg_pixel.color);
            }
        } else {
            if (obj_pixel.bg_priority && bg_pixel.color > 0 && lcdc_bg_wnd_enable()) {
                shade = apply_palette(bgp_, bg_pixel.color);
            } else {
                shade = apply_palette(obj_pixel.palette ? obp1_ : obp0_, obj_pixel.color);
            }
        }
        framebuffer_[ly_ * kScreenWidth + mode3_x_] = shade;

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

bool PPU::lcdc_wnd_tile_map_area() const {
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
