// Window rendering test.
//
// The BG map and the window map hold different tiles, so every pixel shows
// which layer it came from. Each case renders one frame and compares all
// 160x144 pixels against a reference model, and checks the length of mode 3
// on every visible line (+6 dots on lines where the window starts).
//
// The reference model tracks the window's internal line counter separately
// from LY: it only advances on lines where the window was actually drawn.

#include <cppu/ppu.hpp>

#include <cstdint>
#include <cstdio>

using namespace cppu;

namespace {

// Tiles 0-31 are used by the BG, tiles 32-63 by the window.
constexpr int kWindowTileBase = 32;
constexpr int kTileCount = 64;

std::uint8_t tile_lo(int tile, int /*row*/) { return static_cast<std::uint8_t>(tile * 7 + 3); }
std::uint8_t tile_hi(int tile, int row)     { return static_cast<std::uint8_t>(row * 37 + tile); }

int bg_map_tile(int x, int y)  { return (x + y) % 32; }
int wnd_map_tile(int x, int y) { return kWindowTileBase + (x * 3 + y * 5) % 32; }

int tile_color(int tile, int row, int px) {
    const int bit = 7 - px;
    return (((tile_hi(tile, row) >> bit) & 1) << 1) | ((tile_lo(tile, row) >> bit) & 1);
}

struct Case {
    const char*  name;
    std::uint8_t scx;
    std::uint8_t scy;
    std::uint8_t wx;
    std::uint8_t wy;
    bool         swap_maps;         // BG map at 0x1C00 and window map at 0x1800
    int          hide_from = -1;    // clear LCDC.5 for lines [hide_from, hide_to)
    int          hide_to   = -1;
};

int run(const Case& c) {
    PPU ppu;

    for (int t = 0; t < kTileCount; ++t) {
        for (int r = 0; r < 8; ++r) {
            ppu.write_vram(static_cast<std::uint16_t>(t * 16 + r * 2),     tile_lo(t, r));
            ppu.write_vram(static_cast<std::uint16_t>(t * 16 + r * 2 + 1), tile_hi(t, r));
        }
    }

    const int bg_map  = c.swap_maps ? 0x1C00 : 0x1800;
    const int wnd_map = c.swap_maps ? 0x1800 : 0x1C00;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            ppu.write_vram(static_cast<std::uint16_t>(bg_map + y * 32 + x),
                           static_cast<std::uint8_t>(bg_map_tile(x, y)));
            ppu.write_vram(static_cast<std::uint16_t>(wnd_map + y * 32 + x),
                           static_cast<std::uint8_t>(wnd_map_tile(x, y)));
        }
    }

    ppu.write_register(Reg::BGP, 0xE4);
    ppu.write_register(Reg::SCX, c.scx);
    ppu.write_register(Reg::SCY, c.scy);
    ppu.write_register(Reg::WX, c.wx);
    ppu.write_register(Reg::WY, c.wy);

    const std::uint8_t map_bits = c.swap_maps ? lcdc::kBgTileMap : lcdc::kWndTileMap;
    const std::uint8_t lcdc_on = static_cast<std::uint8_t>(
        lcdc::kEnable | lcdc::kWndEnable | lcdc::kBgWndTileData | lcdc::kBgWndEnable | map_bits);
    const std::uint8_t lcdc_hidden = static_cast<std::uint8_t>(lcdc_on & ~lcdc::kWndEnable);

    auto hidden = [&](int line) { return line >= c.hide_from && line < c.hide_to; };

    ppu.write_register(Reg::LCDC, hidden(0) ? lcdc_hidden : lcdc_on);

    // Run one frame, toggling LCDC.5 at the start of each line as requested,
    // and measure mode 3 on every visible line.
    int mode3_dots[kScreenHeight] = {};
    for (int dot = 0; dot < kDotsPerFrame; ++dot) {
        const int line = dot / kDotsPerLine;
        if (dot % kDotsPerLine == 0 && line < kScreenHeight) {
            ppu.write_register(Reg::LCDC, hidden(line) ? lcdc_hidden : lcdc_on);
        }
        if (line < kScreenHeight && ppu.stat_ppu_mode() == 3) {
            ++mode3_dots[line];
        }
        ppu.tick();
    }

    // Reference model.
    int pixel_mismatches = 0;
    int timing_mismatches = 0;
    bool wy_latched = false;
    int window_line = 0;
    const std::uint8_t* fb = ppu.framebuffer();

    for (int y = 0; y < kScreenHeight; ++y) {
        if (y == c.wy) {
            wy_latched = true;
        }
        const bool window_on_line = wy_latched && !hidden(y) && c.wx >= 7 && c.wx <= 166;
        const int  window_x = c.wx - 7;

        for (int x = 0; x < kScreenWidth; ++x) {
            int want;
            if (window_on_line && x >= window_x) {
                const int wx = x - window_x;
                want = tile_color(wnd_map_tile(wx / 8, window_line / 8), window_line % 8, wx % 8);
            } else {
                const int bx = (x + c.scx) & 0xFF;
                const int by = (y + c.scy) & 0xFF;
                want = tile_color(bg_map_tile(bx / 8, by / 8), by % 8, bx % 8);
            }
            const int got = fb[y * kScreenWidth + x];
            if (got != want) {
                if (pixel_mismatches < 5) {
                    std::printf("    pixel mismatch at (%d, %d): got %d, want %d\n", x, y, got, want);
                }
                ++pixel_mismatches;
            }
        }

        const int want_mode3 = 172 + c.scx % 8 + (window_on_line ? 6 : 0);
        if (mode3_dots[y] != want_mode3) {
            if (timing_mismatches < 5) {
                std::printf("    mode 3 on line %d: got %d dots, want %d\n", y, mode3_dots[y], want_mode3);
            }
            ++timing_mismatches;
        }

        if (window_on_line) {
            ++window_line;
        }
    }

    const bool ok = pixel_mismatches == 0 && timing_mismatches == 0;
    std::printf("%-6s %-34s pixel mismatches=%d  timing mismatches=%d\n",
                ok ? "ok" : "FAIL", c.name, pixel_mismatches, timing_mismatches);
    return ok ? 0 : 1;
}

} // namespace

int main() {
    // SCX is a multiple of 8 in the WX=7 case: when the window starts at the
    // left edge, how the window interacts with the SCX fine-scroll discard is
    // not modeled here.
    const Case cases[] = {
        {"WX=47 WY=30, scrolled BG",          13, 77, 47,  30,  false},
        {"WX=7 WY=0 (covers whole screen)",   16, 40, 7,   0,   false},
        {"WX=47 on a FIFO tile boundary",     0,  0,  47,  30,  false},
        {"WX=166 (one column)",               3,  0,  166, 100, false},
        {"WX=167 (off screen)",               3,  0,  167, 10,  false},
        {"WY=200 (never reached)",            0,  0,  50,  200, false},
        {"swapped maps (LCDC.3=1, LCDC.6=0)", 5,  9,  60,  20,  true},
        {"LCDC.5 off for lines 60-69",        0,  0,  30,  40,  false, 60, 70},
    };

    int failures = 0;
    for (const Case& c : cases) {
        if (run(c) != 0) {
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
