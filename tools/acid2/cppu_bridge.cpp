// C interface to a single cppu::PPU instance. See cppu_bridge.h.

#include "cppu_bridge.h"

#include <cppu/ppu.hpp>

namespace {
cppu::PPU g_ppu;
}

extern "C" {

uint8_t cppu_read_vram(uint16_t offset)                { return g_ppu.read_vram(offset); }
void    cppu_write_vram(uint16_t offset, uint8_t value) { g_ppu.write_vram(offset, value); }

uint8_t cppu_read_oam(uint8_t offset)                   { return g_ppu.read_oam(offset); }
void    cppu_write_oam(uint8_t offset, uint8_t value)   { g_ppu.write_oam(offset, value); }
void    cppu_write_oam_dma(uint8_t offset, uint8_t value) { g_ppu.write_oam_dma(offset, value); }

uint8_t cppu_read_register(uint8_t reg) {
    return g_ppu.read_register(static_cast<cppu::Reg>(reg));
}
void cppu_write_register(uint8_t reg, uint8_t value) {
    g_ppu.write_register(static_cast<cppu::Reg>(reg), value);
}

void    cppu_tick(void)            { g_ppu.tick(); }
uint8_t cppu_take_interrupts(void) { return g_ppu.take_interrupts(); }
bool    cppu_frame_ready(void)     { return g_ppu.frame_ready(); }

const uint8_t *cppu_framebuffer(void) { return g_ppu.framebuffer(); }

} // extern "C"
