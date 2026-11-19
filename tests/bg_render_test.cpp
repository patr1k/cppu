// Background rendering test.
//
// Fills VRAM with 32 distinct tiles and a tile map where every entry differs,
// renders one frame, and compares all 160x144 pixels against colors computed
// independently from the tile data. Also checks that mode 3 on the first line
// lasts 172 + (SCX % 8) dots, the hardware length with no window or objects.

#include <cppu/ppu.hpp>

#include <cstdint>
#include <cstdio>

using namespace cppu;

namespace {

constexpr int kNumTiles = 32;

// Tile data bytes are derived from the tile number and row, so every tile row
// produces a different pixel pattern.
std::uint8_t tile_lo(int tile, int /*row*/) { return static_cast<std::uint8_t>(tile * 7 + 3); }
std::uint8_t tile_hi(int tile, int row) { return static_cast<std::uint8_t>(row * 37 + tile); }

// Tile number shown at map position (x, y). Diagonal stripes, so both
// horizontal and vertical scrolling change which tile is visible.
int map_tile(int map_x, int map_y) { return (map_x + map_y) % kNumTiles; }

struct Case {
    const char*  name;
    std::uint8_t scx;
    std::uint8_t scy;
    bool         signed_tiles; // LCDC.4 = 0: tile data at 0x0800-0x17FF, signed index
};

int run(const Case& c) {
    PPU ppu;

    // Tile data. In signed mode use indices 0xE0-0xFF, which are negative
    // (-32..-1) and land in 0x0E00-0x0FFF.
    for (int t = 0; t < kNumTiles; ++t) {
        const int base = c.signed_tiles
            ? 0x1000 + static_cast<std::int8_t>(0xE0 + t) * 16
            : t * 16;
        for (int r = 0; r < 8; ++r) {
            ppu.write_vram(static_cast<std::uint16_t>(base + r * 2),     tile_lo(t, r));
            ppu.write_vram(static_cast<std::uint16_t>(base + r * 2 + 1), tile_hi(t, r));
        }
    }

    // Tile map at 0x1800 (LCDC.3 = 0).
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const int t = map_tile(x, y);
            const int index = c.signed_tiles ? 0xE0 + t : t;
            ppu.write_vram(static_cast<std::uint16_t>(0x1800 + y * 32 + x),
                           static_cast<std::uint8_t>(index));
        }
    }

    ppu.write_register(Reg::BGP, 0xE4); // identity palette: shade == color
    ppu.write_register(Reg::SCX, c.scx);
    ppu.write_register(Reg::SCY, c.scy);

    const std::uint8_t lcdc = static_cast<std::uint8_t>(
        lcdc::kEnable | lcdc::kBgWndEnable | (c.signed_tiles ? 0 : lcdc::kBgWndTileData));
    ppu.write_register(Reg::LCDC, lcdc);

    int mode3_dots = 0;
    for (int dot = 0; dot < kDotsPerFrame; ++dot) {
        if (dot < kDotsPerLine && ppu.stat_ppu_mode() == 3) {
            ++mode3_dots;
        }
        ppu.tick();
    }

    int mismatches = 0;
    const std::uint8_t* fb = ppu.framebuffer();
    for (int y = 0; y < kScreenHeight; ++y) {
        for (int x = 0; x < kScreenWidth; ++x) {
            const int bg_x = (x + c.scx) & 0xFF;
            const int bg_y = (y + c.scy) & 0xFF;
            const int tile = map_tile(bg_x / 8, bg_y / 8);
            const int row  = bg_y % 8;
            const int bit  = 7 - (bg_x % 8);
            const int want = (((tile_hi(tile, row) >> bit) & 1) << 1)
                           |  ((tile_lo(tile, row) >> bit) & 1);
            const int got  = fb[y * kScreenWidth + x];
            if (got != want) {
                if (mismatches < 5) {
                    std::printf("    mismatch at (%d, %d): got %d, want %d\n", x, y, got, want);
                }
                ++mismatches;
            }
        }
    }

    // Mode 3 is 160 output dots + 6 (discarded first fetch) + 6 (first real
    // fetch) + one dot per pixel dropped for fine scroll.
    const int want_mode3 = 172 + c.scx % 8;
    const bool timing_ok = mode3_dots == want_mode3;

    const bool ok = mismatches == 0 && timing_ok;
    std::printf("%-6s %-28s mode3=%d dots (want %d)  mismatches=%d\n",
                ok ? "ok" : "FAIL", c.name, mode3_dots, want_mode3, mismatches);
    return ok ? 0 : 1;
}

} // namespace

int main() {
    const Case cases[] = {
        {"no scroll",                  0,   0,   false},
        {"fine scroll SCX=3",          3,   0,   false},
        {"fine scroll SCX=7 SCY=5",    7,   5,   false},
        {"wrap SCX=250 SCY=200",       250, 200, false},
        {"signed tiles SCX=13 SCY=77", 13,  77,  true},
    };

    int failures = 0;
    for (const Case& c : cases) {
        if (run(c) != 0) {
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
