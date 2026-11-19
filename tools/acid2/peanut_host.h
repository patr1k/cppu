/* Runs a ROM on Peanut-GB with cppu as its PPU. Implemented in
 * peanut_host.c; used by main.cpp. */

#ifndef PEANUT_HOST_H
#define PEANUT_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the emulator on `rom` (kept by pointer; must outlive the run).
 * Returns false and fills `error` if Peanut-GB rejects the cartridge. */
bool host_init(const uint8_t *rom, size_t rom_size, char *error, size_t error_size);

/* Runs until the next frame is ready (cppu reached VBlank, or one frame's
 * worth of time passed with the LCD off). */
void host_run_frame(void);

#ifdef __cplusplus
}
#endif

#endif /* PEANUT_HOST_H */
