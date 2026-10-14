/* Battle City - standalone Switch homebrew (NRO). ROM is embedded. */
#include <switch.h>
#include <stdlib.h>
#include <string.h>
#include "nes.h"

extern const uint8_t rom_data[];
extern const size_t  rom_size;

#define SCALE   3
#define OUT_W   (NES_WIDTH * SCALE)
#define X_OFF   ((1280 - OUT_W) / 2)

static uint8_t map_buttons(u64 k)
{
    uint8_t s = 0;
    if (k & (HidNpadButton_A | HidNpadButton_X))     s |= NES_A;
    if (k & (HidNpadButton_B | HidNpadButton_Y))     s |= NES_B;
    if (k & HidNpadButton_Minus)                     s |= NES_SELECT;
    if (k & HidNpadButton_Plus)                      s |= NES_START;
    if (k & (HidNpadButton_Up | HidNpadButton_StickLUp))       s |= NES_UP;
    if (k & (HidNpadButton_Down | HidNpadButton_StickLDown))   s |= NES_DOWN;
    if (k & (HidNpadButton_Left | HidNpadButton_StickLLeft))   s |= NES_LEFT;
    if (k & (HidNpadButton_Right | HidNpadButton_StickLRight)) s |= NES_RIGHT;
    return s;
}

int main(int argc, char **argv)
{
    Framebuffer fb;
    PadState pad;
    NWindow *win;
    u32 line[OUT_W];
    int frame = 0;

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    if (nes_load(rom_data, rom_size) != 0) {
        consoleInit(NULL);
        printf("ROM failed to load.\nPress + to exit.\n");
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
            consoleUpdate(NULL);
        }
        consoleExit(NULL);
        return 1;
    }

    win = nwindowGetDefault();
    framebufferCreate(&fb, win, 1280, 720, PIXEL_FORMAT_RGBA_8888, 2);
    framebufferMakeLinear(&fb);

    while (appletMainLoop()) {
        u32 stride, *out;
        int y, x;
        u64 held;

        padUpdate(&pad);
        held = padGetButtons(&pad);
        if ((held & HidNpadButton_L) && (held & HidNpadButton_R)) break; /* L+R quits */

        nes_set_pad(map_buttons(held));
        nes_run_frame();

        out = (u32 *)framebufferBegin(&fb, &stride);
        if (frame < 3) {
            for (y = 0; y < 720; y++)
                for (x = 0; x < 1280; x++)
                    ((u32 *)((u8 *)out + y * stride))[x] = 0xFF000000;
        }
        for (y = 0; y < NES_HEIGHT; y++) {
            const uint32_t *src = &nes_fb[y * NES_WIDTH];
            int k;
            for (x = 0; x < NES_WIDTH; x++) {
                uint32_t c = src[x];
                u32 px = 0xFF000000 | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
                line[x * 3] = line[x * 3 + 1] = line[x * 3 + 2] = px;
            }
            for (k = 0; k < SCALE; k++)
                memcpy((u8 *)out + (y * SCALE + k) * stride + X_OFF * 4, line, sizeof(line));
        }
        framebufferEnd(&fb);
        frame++;
    }

    framebufferClose(&fb);
    return 0;
}
