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

    // The window's line counter only advances on lines where it was drawn.
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

            // Window state: reset once per frame, latch WY once per line.
            if (ly_ == 0) {
                wy_latched_ = false;
                wnd_line_ = 0;
            }
            if (ly_ == wy_) {
                wy_latched_ = true;
            }
            in_wnd_ = false;
            break;

        case Mode::Drawing:
            mode3_x_ = 0;
            bg_fifo_.clear();
            obj_fifo_.clear();

            // BG/window fetcher
            bg_fetch_step_ = BgFetchStep::GetTile;
            bg_fetch_dots_ = 0;
            bg_fetch_tile_x_ = 0;
            bg_tile_addr_ = 0;
            bg_tile_lo_ = 0;
            bg_tile_hi_ = 0;
            dummy_fetch_pending_ = true;
            scx_discard_ = scx_ % 8;

            // Object fetch
            obj_fetch_idx_ = -1;
            obj_fetched_mask_ = 0;
            obj_penalty_tile_ = kNoPenaltyTile;
            break;

        case Mode::HBlank:
        case Mode::VBlank:
            break;
    }

    update_stat_line();
}

// ---------------------------------------------------------------------------
// Mode 2: OAM scan
// ---------------------------------------------------------------------------

// Checks one OAM entry every 2 dots (40 entries in 80 dots) and keeps the
// first 10 whose rows cover this line. X isn't checked: off-screen objects
// still count toward the 10.
void PPU::tick_oam_scan() {
    if ((dots_ & 1) == 0) {
        return; // each entry is checked on the second of its 2 dots
    }

    const int entry = dots_ / 2;
    const int y = oam_[static_cast<std::size_t>(entry * 4)];
    const int line = ly_ + kObjYOffset; // LY in OAM's offset coordinates

    if (line >= y && line < y + obj_height() && line_obj_count_ < kMaxObjectsPerLine) {
        const auto base = static_cast<std::size_t>(entry * 4);
        line_objs_[line_obj_count_] = Object{oam_[base], oam_[base + 1], oam_[base + 2], oam_[base + 3]};
        ++line_obj_count_;
    }
}

// ---------------------------------------------------------------------------
// Mode 3: drawing
// ---------------------------------------------------------------------------

void PPU::tick_drawing() {
    // 1. An object fetch in progress freezes everything else until it ends.
    if (obj_fetch_idx_ >= 0) {
        tick_object_fetch();
        return;
    }

    // 2. The window starts here: restart the fetcher on the window's map.
    if (window_starts_here()) {
        start_window();
    }

    // 3. An object is due here: its fetch starts on this dot.
    if (try_start_object_fetch()) {
        return;
    }

    // 4. Fetch BG/window tiles into the BG FIFO.
    tick_bg_fetcher();

    // 5. Output one pixel, if there is one.
    output_pixel();
}

bool PPU::pixel_expected() const {
    return !bg_fifo_.empty() || bg_fetch_step_ == BgFetchStep::Push;
}

// --- Object fetch ------------------------------------------------------------

void PPU::tick_object_fetch() {
    // The BG fetcher and output stay frozen for the whole fetch, which is
    // what makes mode 3 longer. obj_fetch_len_ was set when it started.
    ++obj_fetch_dots_;
    if (obj_fetch_dots_ < obj_fetch_len_) {
        return;
    }

    // Last dot of the fetch.
    merge_object_row(line_objs_[static_cast<std::size_t>(obj_fetch_idx_)]);
    obj_fetched_mask_ = static_cast<std::uint16_t>(obj_fetched_mask_ | (1u << obj_fetch_idx_));
    obj_fetch_idx_ = -1;
}

// Starts fetching the first not-yet-fetched object whose X has been reached.
// Searching in OAM order means objects with the same X are fetched in OAM
// order, which gives DMG overlap priority (see merge_object_row).
bool PPU::try_start_object_fetch() {
    // No fetch during the SCX discard: those BG pixels aren't on screen, so
    // object pixels can't line up with them yet.
    if (!lcdc_obj_enable() || scx_discard_ > 0 || !pixel_expected()) {
        return false;
    }

    for (std::uint8_t i = 0; i < line_obj_count_; ++i) {
        const bool fetched = (obj_fetched_mask_ >> i) & 1;
        // `<=` rather than `==`: objects with X < 8 hang off the left edge
        // and are due as soon as the line starts.
        if (!fetched && line_objs_[i].x <= mode3_x_ + kObjXOffset) {
            obj_fetch_idx_ = static_cast<std::int8_t>(i);
            obj_fetch_dots_ = 1; // this dot is the first dot of the fetch
            obj_fetch_len_ = obj_fetch_length(line_objs_[i]);
            return true;
        }
    }
    return false;
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
    const int sx = obj.x - kObjXOffset;

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

// Reads the object's tile row for this line and merges it into the object
// FIFO. Only transparent slots are filled, so an object already in the FIFO
// keeps its pixels: objects reached earlier (smaller X, or same X and lower
// OAM index) win, which is the DMG's overlap priority.
//
// All VRAM reads happen on this one dot; splitting them across the fetch
// would only matter if the CPU wrote VRAM in the middle of it.
void PPU::merge_object_row(const Object& obj) {
    const int height = obj_height();

    // Row of the object on this line, flipped if needed. Objects always use
    // unsigned tile data at 0x0000; 8x16 objects ignore the tile's low bit,
    // and rows 8-15 fall into the next tile automatically.
    int row = ly_ + kObjYOffset - obj.y;
    if (obj.attrs & oam::kYFlip) {
        row = height - 1 - row;
    }
    const int tile = height == 16 ? (obj.tile & 0xFE) : obj.tile;
    const auto addr = static_cast<std::size_t>(tile * 16 + row * 2);
    const std::uint8_t lo = vram_[addr];
    const std::uint8_t hi = vram_[addr + 1];

    // Line the FIFO up with the next 8 screen pixels.
    while (obj_fifo_.size() < 8) {
        obj_fifo_.push(Pixel{});
    }

    // Objects hanging off the left edge: skip the pixels left of the screen.
    const int skip = mode3_x_ + kObjXOffset - obj.x;
    const bool x_flip = obj.attrs & oam::kXFlip;
    for (int px = skip; px < 8; ++px) {
        const int bit = x_flip ? px : 7 - px;
        const auto color = static_cast<std::uint8_t>((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
        Pixel& slot = obj_fifo_.at(static_cast<std::uint8_t>(px - skip));
        if (slot.color == 0 && color != 0) {
            slot = Pixel{
                color,
                (obj.attrs & oam::kPalette) != 0,
                (obj.attrs & oam::kBgPriority) != 0,
            };
        }
    }
}

// --- Window ------------------------------------------------------------------

// The window starts once LY has matched WY this frame and the next pixel to
// be output is at screen X = WX - 7.
bool PPU::window_starts_here() const {
    return !in_wnd_
        && lcdc_wnd_enable()
        && wy_latched_
        && mode3_x_ + 7 == wx_
        && pixel_expected();
}

// Throws away the BG pixels still in the FIFO and restarts the fetcher at
// the window's first tile. The empty FIFO stalls output while that tile is
// fetched, which is the window's 6-dot cost.
void PPU::start_window() {
    in_wnd_ = true;
    bg_fifo_.clear();
    bg_fetch_step_ = BgFetchStep::GetTile;
    bg_fetch_dots_ = 0;
    bg_fetch_tile_x_ = 0;
    scx_discard_ = 0; // fine scroll only applies to the BG
}

// --- BG/window fetcher -------------------------------------------------------

void PPU::tick_bg_fetcher() {
    switch (bg_fetch_step_) {
        case BgFetchStep::GetTile:
            if (bg_fetch_dots_ == 1) {
                bg_tile_addr_ = bg_tile_row_addr();
            }
            break;

        case BgFetchStep::GetTileDataLow:
            if (bg_fetch_dots_ == 1) {
                bg_tile_lo_ = vram_[bg_tile_addr_];
            }
            break;

        case BgFetchStep::GetTileDataHigh:
            if (bg_fetch_dots_ == 1) {
                bg_tile_hi_ = vram_[bg_tile_addr_ + 1u];
            }
            break;

        case BgFetchStep::Push:
            // Wait until the FIFO is empty, then push the whole row at once.
            if (bg_fifo_.empty()) {
                push_bg_row();
                ++bg_fetch_tile_x_;
                bg_fetch_step_ = BgFetchStep::GetTile;
                bg_fetch_dots_ = 0;
            }
            return;
    }

    // GetTile, GetTileDataLow and GetTileDataHigh each take 2 dots.
    ++bg_fetch_dots_;
    if (bg_fetch_dots_ < 2) {
        return;
    }
    bg_fetch_dots_ = 0;

    if (bg_fetch_step_ == BgFetchStep::GetTileDataHigh && dummy_fetch_pending_) {
        // The first tile fetch of each line is done twice: throw away this
        // one's result and fetch the same tile again.
        dummy_fetch_pending_ = false;
        bg_fetch_step_ = BgFetchStep::GetTile;
    } else {
        bg_fetch_step_ = static_cast<BgFetchStep>(static_cast<std::uint8_t>(bg_fetch_step_) + 1);
    }
}

// Looks up the tile at the fetcher's position in the BG or window tile map
// and returns the VRAM offset of the row of it that this line shows.
std::uint16_t PPU::bg_tile_row_addr() const {
    int map;
    int map_row;
    int map_col;
    int tile_row;
    if (in_wnd_) {
        // The window isn't scrolled; it has its own line counter.
        map = lcdc_wnd_tile_map_area() ? kTileMap1 : kTileMap0;
        map_row = wnd_line_ / 8;
        map_col = bg_fetch_tile_x_;
        tile_row = wnd_line_ % 8;
    } else {
        // The BG is scrolled by SCX/SCY and wraps around the 32x32 map.
        const int bg_y = (ly_ + scy_) & 0xFF;
        map = lcdc_bg_tile_map_area() ? kTileMap1 : kTileMap0;
        map_row = bg_y / 8;
        map_col = (scx_ / 8 + bg_fetch_tile_x_) & 0x1F;
        tile_row = bg_y % 8;
    }

    const std::uint8_t tile = vram_[static_cast<std::size_t>(map + map_row * 32 + map_col)];
    const int row_offset = tile_row * 2;

    if (lcdc_bg_wnd_tile_data_area()) {
        // LCDC.4 = 1: unsigned tile index, tiles at 0x0000-0x0FFF.
        return static_cast<std::uint16_t>(tile * 16 + row_offset);
    }
    // LCDC.4 = 0: signed tile index around 0x1000, tiles at 0x0800-0x17FF.
    return static_cast<std::uint16_t>(0x1000 + static_cast<std::int8_t>(tile) * 16 + row_offset);
}

// Decodes the fetched tile row into 8 pixels, leftmost (bit 7) first.
void PPU::push_bg_row() {
    for (int bit = 7; bit >= 0; --bit) {
        const auto color = static_cast<std::uint8_t>(
            (((bg_tile_hi_ >> bit) & 1) << 1) | ((bg_tile_lo_ >> bit) & 1));
        bg_fifo_.push(Pixel{color, false, false});
    }
}

// --- Pixel output ------------------------------------------------------------

void PPU::output_pixel() {
    if (bg_fifo_.empty()) {
        return; // stalled: waiting for the fetcher
    }
    const Pixel bg = bg_fifo_.pop();

    // Fine scroll: drop the first SCX % 8 BG pixels of the line.
    if (scx_discard_ > 0) {
        --scx_discard_;
        return;
    }

    // The object FIFO is popped in step with the screen, not with the BG
    // FIFO's discarded pixels.
    const Pixel obj = obj_fifo_.empty() ? Pixel{} : obj_fifo_.pop();

    framebuffer_[static_cast<std::size_t>(ly_ * kScreenWidth + mode3_x_)] = mix_pixel(bg, obj);
    ++mode3_x_;
}

// Picks the shade (0-3) shown for one screen pixel.
std::uint8_t PPU::mix_pixel(const Pixel& bg, const Pixel& obj) const {
    // With LCDC.0 off, the BG and window are blank (white) and count as
    // color 0, so objects always win.
    const bool bg_on = lcdc_bg_wnd_enable();
    const std::uint8_t bg_shade = bg_on ? apply_palette(bgp_, bg.color) : 0;

    if (obj.color == 0) {
        return bg_shade; // transparent object pixel (or no object)
    }
    if (obj.bg_priority && bg_on && bg.color != 0) {
        return bg_shade; // object is behind BG colors 1-3
    }
    return apply_palette(obj.palette ? obp1_ : obp0_, obj.color);
}

std::uint8_t PPU::apply_palette(std::uint8_t palette, std::uint8_t color) const {
    return static_cast<std::uint8_t>((palette >> (color * 2)) & 3);
}

// ---------------------------------------------------------------------------
// Memory and register access (CPU side)
// ---------------------------------------------------------------------------

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
