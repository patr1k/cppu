/* C interface to a single cppu::PPU instance, so the C99 Peanut-GB host
 * (peanut_host.c) can drive cppu. Implemented in cppu_bridge.cpp. */

#ifndef CPPU_BRIDGE_H
#define CPPU_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t cppu_read_vram(uint16_t offset);
void    cppu_write_vram(uint16_t offset, uint8_t value);

uint8_t cppu_read_oam(uint8_t offset);
void    cppu_write_oam(uint8_t offset, uint8_t value);
void    cppu_write_oam_dma(uint8_t offset, uint8_t value);

/* reg is the offset from 0xFF40 (0x0-0xB), matching cppu::Reg. */
uint8_t cppu_read_register(uint8_t reg);
void    cppu_write_register(uint8_t reg, uint8_t value);

void    cppu_tick(void);
uint8_t cppu_take_interrupts(void);
bool    cppu_frame_ready(void);

/* 160x144 shades (0-3), row-major. */
const uint8_t *cppu_framebuffer(void);

#ifdef __cplusplus
}
#endif

#endif /* CPPU_BRIDGE_H */
