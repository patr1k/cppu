// CPU access test.
//
// Checks when the CPU side of the PPU (read_vram / write_vram / read_oam /
// write_oam) is locked out, and the PPU's two OAM DMA pieces:
//
//   - LCD off: VRAM and OAM are always accessible
//   - LCD on:  VRAM is locked in mode 3; OAM is locked in modes 2 and 3.
//              Locked reads return 0xFF and locked writes are ignored.
//   - write_oam_dma() writes OAM in every mode
//   - the DMA register (0xFF46) reads back the last value written

#include <cppu/ppu.hpp>

#include <cstdint>
#include <cstdio>

using namespace cppu;

namespace {

int failures = 0;

void check(const char* what, bool ok) {
    std::printf("%-6s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

// Ticks until the PPU is in `mode` (0-3). Starts from wherever it is.
void run_to_mode(PPU& ppu, int mode) {
    for (int i = 0; i < kDotsPerFrame && ppu.stat_ppu_mode() != mode; ++i) {
        ppu.tick();
    }
}

constexpr std::uint16_t kVramAddr = 0x0123;
constexpr std::uint8_t  kOamAddr  = 0x42;

// Checks reads and writes of VRAM and OAM in the PPU's current mode.
// Each probe writes a fresh value with the normal API, then checks it with
// the PPU's state unchanged (no ticks in between).
void probe(PPU& ppu, const char* label, bool vram_open, bool oam_open, std::uint8_t seed) {
    char what[160];

    // Reads: seed the memory first through a path that's never locked.
    // (VRAM has none, so read whatever is there and compare to 0xFF only
    // when locked.)
    ppu.write_oam_dma(kOamAddr, seed);
    const std::uint8_t oam_read = ppu.read_oam(kOamAddr);
    std::snprintf(what, sizeof(what), "%s: OAM read %s", label, oam_open ? "works" : "returns 0xFF");
    check(what, oam_open ? oam_read == seed : oam_read == 0xFF);

    if (!vram_open) {
        std::snprintf(what, sizeof(what), "%s: VRAM read returns 0xFF", label);
        check(what, ppu.read_vram(kVramAddr) == 0xFF);
    }

    // Writes: write a new value, then look at memory once access is open
    // again (LCD off) to see whether it landed.
    const auto new_vram = static_cast<std::uint8_t>(seed + 1);
    const auto new_oam  = static_cast<std::uint8_t>(seed + 2);
    ppu.write_vram(kVramAddr, new_vram);
    ppu.write_oam(kOamAddr, new_oam);

    const std::uint8_t lcdc = ppu.read_register(Reg::LCDC);
    ppu.write_register(Reg::LCDC, static_cast<std::uint8_t>(lcdc & ~lcdc::kEnable));
    const bool vram_landed = ppu.read_vram(kVramAddr) == new_vram;
    const bool oam_landed  = ppu.read_oam(kOamAddr) == new_oam;

    std::snprintf(what, sizeof(what), "%s: VRAM write %s", label, vram_open ? "lands" : "is ignored");
    check(what, vram_landed == vram_open);
    std::snprintf(what, sizeof(what), "%s: OAM write %s", label, oam_open ? "lands" : "is ignored");
    check(what, oam_landed == oam_open);
}

// A fresh PPU with the LCD on, run until it reaches `mode`.
void fresh_in_mode(PPU& ppu, int mode) {
    ppu = PPU{};
    ppu.write_register(Reg::LCDC, lcdc::kEnable | lcdc::kBgWndEnable);
    run_to_mode(ppu, mode);
}

} // namespace

int main() {
    PPU ppu;

    // LCD off. The PPU starts with LCDC = 0 but STAT's mode bits at 2, so
    // this also checks that locking depends on the LCD being on.
    probe(ppu, "LCD off", true, true, 0x10);

    fresh_in_mode(ppu, 2);
    probe(ppu, "mode 2 (OAM scan)", true, false, 0x20);

    fresh_in_mode(ppu, 3);
    probe(ppu, "mode 3 (drawing)", false, false, 0x30);

    fresh_in_mode(ppu, 0);
    probe(ppu, "mode 0 (HBlank)", true, true, 0x40);

    fresh_in_mode(ppu, 1);
    probe(ppu, "mode 1 (VBlank)", true, true, 0x50);

    // write_oam_dma lands in the locked modes too.
    for (int mode : {2, 3}) {
        fresh_in_mode(ppu, mode);
        ppu.write_oam_dma(0x9F, 0xA5);
        ppu.write_register(Reg::LCDC, 0);
        check(mode == 2 ? "write_oam_dma lands in mode 2" : "write_oam_dma lands in mode 3",
              ppu.read_oam(0x9F) == 0xA5);
    }

    // Out-of-range OAM offsets are still ignored by write_oam_dma.
    ppu = PPU{};
    ppu.write_oam_dma(0xA0, 0x77);
    check("write_oam_dma ignores offsets past 0x9F", ppu.read_oam(0xA0) == 0xFF);

    // The DMA register reads back what was written.
    ppu = PPU{};
    ppu.write_register(Reg::DMA, 0xC1);
    check("DMA register reads back the last value written", ppu.read_register(Reg::DMA) == 0xC1);

    return failures == 0 ? 0 : 1;
}
