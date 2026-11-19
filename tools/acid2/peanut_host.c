/* Hosts Peanut-GB with cppu as its PPU.
 *
 * Peanut-GB is built with PEANUT_GB_EXTERNAL_PPU (our patch, see
 * third_party/peanut_gb/cppu-integration.patch), which routes VRAM, OAM and
 * the LCD registers to the gb_ext_ppu_* hooks below and lets us drive the
 * LCD timing. This file implements those hooks on top of cppu, including
 * OAM DMA, which cppu leaves to the host. */

#define ENABLE_LCD 0              /* cppu draws, not Peanut-GB */
#define ENABLE_SOUND 0
#define PEANUT_GB_EXTERNAL_PPU 1
#include "third_party/peanut_gb/peanut_gb.h"

#include "cppu_bridge.h"
#include "peanut_host.h"

#include <stdio.h>
#include <stdlib.h>

static struct gb_s gb;
static const uint8_t *rom_data;
static size_t rom_length;

/* OAM DMA in progress. Copies one byte every 4 dots, starting 4 dots after
 * the write to 0xFF46 (the same scheme as the example in cppu's README).
 * Simplification: the CPU isn't restricted to HRAM during the transfer. */
static struct {
    bool active;
    uint16_t source;
    unsigned index;
    unsigned dots;
} dma;

/* Dots spent with the LCD off, so frames keep coming while it's off. */
static uint_fast32_t lcd_off_dots;

/* --- Peanut-GB callbacks --------------------------------------------------- */

static uint8_t rom_read(struct gb_s *g, const uint_fast32_t addr) {
    (void)g;
    return addr < rom_length ? rom_data[addr] : 0xFF;
}

/* No cartridge RAM (dmg-acid2 is a plain 32 KiB ROM). */
static uint8_t cart_ram_read(struct gb_s *g, const uint_fast32_t addr) {
    (void)g;
    (void)addr;
    return 0xFF;
}

static void cart_ram_write(struct gb_s *g, const uint_fast32_t addr, const uint8_t val) {
    (void)g;
    (void)addr;
    (void)val;
}

/* Peanut-GB treats errors as unrecoverable, so this must not return. */
static void on_error(struct gb_s *g, const enum gb_error_e err, const uint16_t addr) {
    (void)g;
    fprintf(stderr, "Peanut-GB error %d at address 0x%04X\n", (int)err, addr);
    abort();
}

/* --- External PPU hooks ---------------------------------------------------- */

uint8_t gb_ext_ppu_read(struct gb_s *g, uint_fast16_t addr) {
    (void)g;
    if (addr >= 0x8000 && addr <= 0x9FFF)
        return cppu_read_vram((uint16_t)(addr - 0x8000));
    if (addr >= 0xFE00 && addr <= 0xFE9F)
        return cppu_read_oam((uint8_t)(addr - 0xFE00));
    return cppu_read_register((uint8_t)(addr - 0xFF40));
}

void gb_ext_ppu_write(struct gb_s *g, uint_fast16_t addr, uint8_t val) {
    (void)g;
    if (addr >= 0x8000 && addr <= 0x9FFF) {
        cppu_write_vram((uint16_t)(addr - 0x8000), val);
    } else if (addr >= 0xFE00 && addr <= 0xFE9F) {
        cppu_write_oam((uint8_t)(addr - 0xFE00), val);
    } else {
        const uint8_t reg = (uint8_t)(addr - 0xFF40);
        cppu_write_register(reg, val);
        if (addr == 0xFF46) {
            /* Start (or restart) OAM DMA from val << 8. */
            dma.active = true;
            dma.source = (uint16_t)(val << 8);
            dma.index = 0;
            dma.dots = 4;
        }
    }
}

static void tick_dma(struct gb_s *g) {
    if (!dma.active || --dma.dots > 0)
        return;
    cppu_write_oam_dma((uint8_t)dma.index, __gb_read(g, (uint16_t)(dma.source + dma.index)));
    dma.dots = 4;
    if (++dma.index == 160)
        dma.active = false;
}

void gb_ext_ppu_tick(struct gb_s *g, uint_fast16_t cycles) {
    for (uint_fast16_t i = 0; i < cycles; ++i) {
        tick_dma(g);
        cppu_tick();
    }

    /* Merge VBlank / STAT interrupts into IF. */
    g->hram_io[IO_IF] |= cppu_take_interrupts();

    if (cppu_frame_ready())
        g->gb_frame = true;

    /* With the LCD off, cppu never reaches VBlank; still end a frame
     * every 70224 dots so the front-end keeps running. */
    if (!(cppu_read_register(0x0) & 0x80)) {
        lcd_off_dots += cycles;
        if (lcd_off_dots >= LCD_FRAME_CYCLES) {
            lcd_off_dots -= LCD_FRAME_CYCLES;
            g->gb_frame = true;
        }
    }
}

/* --- Public API ------------------------------------------------------------ */

bool host_init(const uint8_t *rom, size_t rom_size, char *error, size_t error_size) {
    rom_data = rom;
    rom_length = rom_size;

    const enum gb_init_error_e ret =
        gb_init(&gb, rom_read, cart_ram_read, cart_ram_write, on_error, NULL);
    if (ret != GB_INIT_NO_ERROR) {
        snprintf(error, error_size, "Peanut-GB couldn't load the ROM (gb_init error %d)", (int)ret);
        return false;
    }

    /* gb_reset() sets up the post-boot-ROM state, but its LCDC write goes
     * straight into Peanut-GB's own register array. Turn cppu's LCD on the
     * way the boot ROM leaves it (BGP/OBP0/OBP1 already went through
     * __gb_write, so cppu has those). */
    __gb_write(&gb, 0xFF40, 0x91);
    return true;
}

void host_run_frame(void) {
    gb_run_frame(&gb);
}
