// Object rendering test.
//
// Each case places objects over a known BG pattern, renders one frame and
// compares all 160x144 pixels against a reference model of the DMG rules:
//
//   - OAM scan keeps the first 10 objects (in OAM order) whose rows cover LY,
//     whatever their X
//   - among objects with a non-transparent pixel at a screen position, the one
//     with the smallest X wins; equal X -> the lower OAM index wins
//   - the winning pixel is hidden behind BG colors 1-3 if its BG-priority bit
//     is set; otherwise it's drawn through OBP0 or OBP1
//   - LCDC.1 off hides objects; LCDC.0 off blanks the BG to white but objects
//     are still drawn (and always win)
//
// It also checks mode 3 on every visible line: 172 + (SCX % 8), plus 6-11
// dots per object fetched (every selected object with X < 168), following
// Pan Docs' rule; see object_penalty() below.

#include <cppu/ppu.hpp>

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace cppu;

namespace {

// --- BG: tiles 0-31, same scheme as bg_render_test ---------------------------
std::uint8_t bg_lo(int tile, int /*row*/) { return static_cast<std::uint8_t>(tile * 7 + 3); }
std::uint8_t bg_hi(int tile, int row)     { return static_cast<std::uint8_t>(row * 37 + tile); }
int bg_map_tile(int x, int y) { return (x + y) % 32; }

// --- Object tiles: 64-127. Patterns include color 0, so transparency matters.
constexpr int kObjTileFirst = 64;
constexpr int kObjTileLast  = 127;
std::uint8_t obj_lo(int tile, int row) { return static_cast<std::uint8_t>((tile * 29 + row * 11) ^ 0x5A); }
std::uint8_t obj_hi(int tile, int row) { return static_cast<std::uint8_t>((tile * 17 + row * 23) ^ 0xA5); }

int color_of(std::uint8_t lo, std::uint8_t hi, int bit) {
    return (((hi >> bit) & 1) << 1) | ((lo >> bit) & 1);
}

int apply_palette(int palette, int color) { return (palette >> (color * 2)) & 3; }

// Distinct palettes, so using the wrong one shows up as a pixel mismatch.
constexpr std::uint8_t kBgp  = 0xE4; // identity
constexpr std::uint8_t kObp0 = 0xD2; // 0->2 1->0 2->1 3->3
constexpr std::uint8_t kObp1 = 0x1B; // reversed

struct Obj {
    int y, x, tile, attrs;
};

struct Case {
    const char*      name;
    std::vector<Obj> objs;         // OAM entries 0..n-1
    bool             tall = false; // LCDC.2: 8x16 objects
    bool             obj_enable = true;
    bool             bg_enable = true;
    std::uint8_t     scx = 0;
    std::uint8_t     scy = 0;
};

// Extra mode 3 dots for the objects selected on one line (no window).
//
// Objects are fetched in the order the screen reaches them: an object's
// fetch starts at screen X max(X - 8, 0), and objects due at the same X are
// fetched in OAM order. Each costs:
//   - 11 if X == 0
//   - 6 if an earlier object already paid for the BG tile under its
//     leftmost pixel
//   - otherwise 6 + max(0, 5 - p), p = its leftmost pixel's position in that
//     BG tile
int object_penalty(const Case& c, const std::vector<int>& selected) {
    if (!c.obj_enable) {
        return 0;
    }

    std::vector<int> order;
    for (int i : selected) {
        if (c.objs[static_cast<std::size_t>(i)].x < 168) {
            order.push_back(i);
        }
    }
    auto start = [&](int i) {
        const int x = c.objs[static_cast<std::size_t>(i)].x;
        return x > 8 ? x - 8 : 0;
    };
    // Stable insertion sort by start position keeps OAM order for ties.
    for (std::size_t a = 1; a < order.size(); ++a) {
        for (std::size_t b = a; b > 0 && start(order[b]) < start(order[b - 1]); --b) {
            std::swap(order[b], order[b - 1]);
        }
    }

    int total = 0;
    bool have_tile = false;
    int last_tile = 0;
    for (int i : order) {
        const int x = c.objs[static_cast<std::size_t>(i)].x;
        if (x == 0) {
            total += 11;
            continue;
        }
        const int bg_x = x - 8 + c.scx;                 // BG pixel under the leftmost pixel
        const int tile = (bg_x + 256) / 8;              // +256 keeps it non-negative
        const int p = (bg_x + 256) % 8;
        if (have_tile && tile == last_tile) {
            total += 6;
        } else {
            total += 6 + (p < 5 ? 5 - p : 0);
            have_tile = true;
            last_tile = tile;
        }
    }
    return total;
}

int run(const Case& c) {
    PPU ppu;

    for (int t = 0; t < 32; ++t) {
        for (int r = 0; r < 8; ++r) {
            ppu.write_vram(static_cast<std::uint16_t>(t * 16 + r * 2),     bg_lo(t, r));
            ppu.write_vram(static_cast<std::uint16_t>(t * 16 + r * 2 + 1), bg_hi(t, r));
        }
    }
    for (int t = kObjTileFirst; t <= kObjTileLast; ++t) {
        for (int r = 0; r < 8; ++r) {
            ppu.write_vram(static_cast<std::uint16_t>(t * 16 + r * 2),     obj_lo(t, r));
            ppu.write_vram(static_cast<std::uint16_t>(t * 16 + r * 2 + 1), obj_hi(t, r));
        }
    }
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            ppu.write_vram(static_cast<std::uint16_t>(0x1800 + y * 32 + x),
                           static_cast<std::uint8_t>(bg_map_tile(x, y)));
        }
    }

    // Unused OAM entries stay at Y = 0, which is never on screen.
    for (std::size_t i = 0; i < c.objs.size(); ++i) {
        const Obj& o = c.objs[i];
        const auto base = static_cast<std::uint8_t>(i * 4);
        ppu.write_oam(base,     static_cast<std::uint8_t>(o.y));
        ppu.write_oam(static_cast<std::uint8_t>(base + 1), static_cast<std::uint8_t>(o.x));
        ppu.write_oam(static_cast<std::uint8_t>(base + 2), static_cast<std::uint8_t>(o.tile));
        ppu.write_oam(static_cast<std::uint8_t>(base + 3), static_cast<std::uint8_t>(o.attrs));
    }

    ppu.write_register(Reg::BGP,  kBgp);
    ppu.write_register(Reg::OBP0, kObp0);
    ppu.write_register(Reg::OBP1, kObp1);
    ppu.write_register(Reg::SCX,  c.scx);
    ppu.write_register(Reg::SCY,  c.scy);

    int lcdc = lcdc::kEnable | lcdc::kBgWndTileData;
    if (c.bg_enable)  lcdc |= lcdc::kBgWndEnable;
    if (c.obj_enable) lcdc |= lcdc::kObjEnable;
    if (c.tall)       lcdc |= lcdc::kObjSize;
    ppu.write_register(Reg::LCDC, static_cast<std::uint8_t>(lcdc));

    int mode3_dots[kScreenHeight] = {};
    for (int dot = 0; dot < kDotsPerFrame; ++dot) {
        const int line = dot / kDotsPerLine;
        if (line < kScreenHeight && ppu.stat_ppu_mode() == 3) {
            ++mode3_dots[line];
        }
        ppu.tick();
    }

    // --- Reference model -----------------------------------------------------
    const int height = c.tall ? 16 : 8;
    const std::uint8_t* fb = ppu.framebuffer();
    int pixel_mismatches = 0;
    int timing_mismatches = 0;

    for (int y = 0; y < kScreenHeight; ++y) {
        // OAM scan: first 10 objects covering this line, in OAM order.
        std::vector<int> selected;
        for (int i = 0; i < static_cast<int>(c.objs.size()) && selected.size() < 10; ++i) {
            const int top = c.objs[static_cast<std::size_t>(i)].y - 16;
            if (y >= top && y < top + height) {
                selected.push_back(i);
            }
        }

        for (int x = 0; x < kScreenWidth; ++x) {
            const int bx = (x + c.scx) & 0xFF;
            const int by = (y + c.scy) & 0xFF;
            const int bg_tile = bg_map_tile(bx / 8, by / 8);
            const int bg_color = c.bg_enable
                ? color_of(bg_lo(bg_tile, by % 8), bg_hi(bg_tile, by % 8), 7 - bx % 8)
                : 0;

            // Find the winning object pixel.
            int win = -1;
            int win_color = 0;
            if (c.obj_enable) {
                for (int i : selected) {
                    const Obj& o = c.objs[static_cast<std::size_t>(i)];
                    const int left = o.x - 8;
                    if (x < left || x >= left + 8) continue;

                    int row = y - (o.y - 16);
                    if (o.attrs & oam::kYFlip) row = height - 1 - row;
                    const int tile = c.tall ? ((o.tile & 0xFE) + row / 8) : o.tile;
                    const int px = x - left;
                    const int bit = (o.attrs & oam::kXFlip) ? px : 7 - px;
                    const int color = color_of(obj_lo(tile, row % 8), obj_hi(tile, row % 8), bit);
                    if (color == 0) continue;

                    const bool better = win < 0
                        || o.x < c.objs[static_cast<std::size_t>(win)].x; // equal X: keep lower index
                    if (better) {
                        win = i;
                        win_color = color;
                    }
                }
            }

            int want;
            const int bg_shade = c.bg_enable ? apply_palette(kBgp, bg_color) : 0;
            if (win >= 0) {
                const Obj& o = c.objs[static_cast<std::size_t>(win)];
                const bool behind = (o.attrs & oam::kBgPriority) && bg_color != 0;
                want = behind ? bg_shade
                              : apply_palette((o.attrs & oam::kPalette) ? kObp1 : kObp0, win_color);
            } else {
                want = bg_shade;
            }

            const int got = fb[y * kScreenWidth + x];
            if (got != want) {
                if (pixel_mismatches < 5) {
                    std::printf("    pixel mismatch at (%d, %d): got %d, want %d\n", x, y, got, want);
                }
                ++pixel_mismatches;
            }
        }

        const int want_mode3 = 172 + c.scx % 8 + object_penalty(c, selected);
        if (mode3_dots[y] != want_mode3) {
            if (timing_mismatches < 5) {
                std::printf("    mode 3 on line %d: got %d dots, want %d\n", y, mode3_dots[y], want_mode3);
            }
            ++timing_mismatches;
        }
    }

    const bool ok = pixel_mismatches == 0 && timing_mismatches == 0;
    std::printf("%-6s %-40s pixel mismatches=%d  timing mismatches=%d\n",
                ok ? "ok" : "FAIL", c.name, pixel_mismatches, timing_mismatches);
    return ok ? 0 : 1;
}

} // namespace

int main() {
    using namespace oam;
    constexpr int T = kObjTileFirst;

    std::vector<Case> cases;

    cases.push_back({"positions incl. partly off each edge", {
        {30,  20,  T + 0, 0},   // fully on screen
        {50,  4,   T + 1, 0},   // partly off the left edge
        {70,  164, T + 2, 0},   // partly off the right edge
        {10,  80,  T + 3, 0},   // partly off the top
        {156, 100, T + 4, 0},   // partly off the bottom
        {0,   60,  T + 5, 0},   // Y = 0: never visible
        {90,  0,   T + 6, 0},   // X = 0: never visible
        {110, 168, T + 7, 0},   // X = 168: never visible
    }});

    cases.push_back({"fine scroll SCX=5 with left-edge object", {
        {40, 3,  T + 8, 0},
        {40, 30, T + 9, 0},
    }, false, true, true, 5, 11});

    cases.push_back({"X flip, Y flip, both", {
        {30, 40, T + 10, kXFlip},
        {30, 60, T + 10, kYFlip},
        {30, 80, T + 10, kXFlip | kYFlip},
        {30, 100, T + 10, 0},
    }});

    cases.push_back({"8x16 objects, odd tile index, Y flip", {
        {40, 40, T + 13, 0},          // uses tiles T+12 / T+13
        {40, 70, T + 20, kYFlip},
        {8,  100, T + 22, 0},         // partly off the top
    }, true});

    cases.push_back({"overlap: smaller X wins, equal X -> lower OAM index", {
        {50, 44, T + 30, 0},          // X=44 (OAM 0)
        {50, 40, T + 31, 0},          // X=40 (OAM 1) wins over OAM 0 where both are opaque
        {80, 60, T + 32, 0},          // equal X: OAM 2 ...
        {80, 60, T + 33, 0},          // ... beats OAM 3
    }});

    {
        Case c{"11 objects on a line; X=0 counts toward 10", {}};
        c.objs.push_back({60, 0, T + 40, 0});                  // off screen, still selected
        for (int i = 0; i < 10; ++i) {
            c.objs.push_back({60, 12 + i * 14, T + 41 + i, 0}); // the last one is dropped
        }
        cases.push_back(c);
    }

    cases.push_back({"OBP1 palette", {
        {30, 30, T + 50, kPalette},
        {30, 50, T + 51, 0},
    }});

    cases.push_back({"BG priority bit", {
        {30, 30, T + 52, kBgPriority},
        {30, 50, T + 53, kBgPriority | kPalette},
    }});

    cases.push_back({"LCDC.1 off: no objects, no extra dots", {
        {30, 30, T + 54, 0},
        {30, 50, T + 55, 0},
    }, false, false});

    cases.push_back({"LCDC.0 off: objects drawn over white", {
        {30, 30, T + 56, 0},
        {30, 50, T + 57, kBgPriority},
    }, false, true, false});

    // --- Timing: one object per line at each position p = 0..7 within a BG
    // tile, so each line shows one row of the 6-11 dot table.
    for (int scx : {0, 3}) {
        Case c{scx == 0 ? "timing: each position in a tile, SCX=0"
                        : "timing: each position in a tile, SCX=3", {}};
        c.scx = static_cast<std::uint8_t>(scx);
        for (int p = 0; p < 8; ++p) {
            // Leftmost pixel at BG X 32 + p (inside the 5th BG tile).
            const int screen_x = 32 + p - scx;
            c.objs.push_back({20 + p * 12, screen_x + 8, T + 20 + p, 0});
        }
        cases.push_back(c);
    }

    cases.push_back({"timing: only the first object in a tile pays the wait", {
        {30, 32, T + 1, 0},   // p=0: 11
        {30, 35, T + 2, 0},   // same tile: 6
        {50, 37, T + 3, 0},   // p=5: 6
        {50, 41, T + 4, 0},   // next tile, p=1: 10
        {70, 40, T + 5, 0},   // p=0: 11
        {70, 40, T + 6, 0},   // same X, same tile: 6
    }});

    cases.push_back({"timing: X=0 costs 11 regardless of SCX", {
        {30, 0, T + 7, 0},
        {50, 0, T + 8, 0},
        {50, 60, T + 9, 0},
    }, false, true, true, 6});

    int failures = 0;
    for (const Case& c : cases) {
        if (run(c) != 0) {
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
