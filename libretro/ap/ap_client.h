/* Built-in Archipelago client for SMZ3 inside the Snes9x libretro core. */
#ifndef AP_CLIENT_H
#define AP_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include "libretro.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Implemented by the emulator glue (ap_glue.cpp, or a test harness).
 * Addresses use the same layout as the official SNI-based client:
 *   0x000000-0xDFFFFF  ROM (file offset)
 *   0xE00000-0xEFFFFF  cartridge save RAM
 *   0xF50000-0xF6FFFF  work RAM
 * Both return 1 on success, 0 if any byte is out of range. */
int ap_mem_read(uint32_t addr, uint8_t *out, size_t len);
int ap_mem_write(uint32_t addr, const uint8_t *in, size_t len);

/* Called by the core. ap_start is a no-op unless the loaded ROM is SMZ3. */
void ap_start(retro_environment_t env, const char *content_path);
void ap_frame(void);
void ap_stop(void);

/* Text to draw on the game picture this frame (NULL when nothing to show).
 * Called from the emulator thread. */
const char *ap_overlay_text(void);

/* Draws the overlay onto an RGB565 frame (implemented in ap_glue.cpp). */
void ap_draw_overlay(uint16_t *screen, int pitch_pixels, int width, int height);

#ifdef __cplusplus
}
#endif

#endif
