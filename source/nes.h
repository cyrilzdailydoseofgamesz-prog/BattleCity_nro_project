#ifndef NES_CORE_H
#define NES_CORE_H

#include <stdint.h>
#include <stddef.h>

#define NES_WIDTH  256
#define NES_HEIGHT 240

/* pad bits (same order the NES shifts them out) */
#define NES_A      0x01
#define NES_B      0x02
#define NES_SELECT 0x04
#define NES_START  0x08
#define NES_UP     0x10
#define NES_DOWN   0x20
#define NES_LEFT   0x40
#define NES_RIGHT  0x80

/* 0x00RRGGBB per pixel, NES_WIDTH * NES_HEIGHT */
extern uint32_t nes_fb[NES_WIDTH * NES_HEIGHT];

/* returns 0 on success, -1 bad header, -2 unsupported mapper, -3 truncated */
int  nes_load(const uint8_t *data, size_t size);
void nes_reset(void);
void nes_set_pad(uint8_t state);
void nes_run_frame(void);

#endif
