/* Battle City - standalone Switch homebrew (NRO). ROM is embedded. */
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include "nes.h"

extern const uint8_t rom_data[];
extern const size_t  rom_size;

#define SCALE   3
#define OUT_W   (NES_WIDTH * SCALE)
#define X_OFF   ((1280 - OUT_W) / 2)

/* ---------------------------------------------------------------- audio */
#define ABUF_COUNT  4
#define ABUF_FRAMES 1024                       /* stereo frames per buffer */
#define ABUF_BYTES  (ABUF_FRAMES * 2 * sizeof(s16))   /* 4096, page aligned */
#define FIFO_MAX    3072

static AudioOutBuffer abuf[ABUF_COUNT];
static s16 *adata[ABUF_COUNT];
static AudioOutBuffer *free_list[ABUF_COUNT];
static int free_n;
static AudioOutBuffer *sub_q[ABUF_COUNT];      /* submitted, in play order */
static int sub_head, sub_n;
static s16 fifo[FIFO_MAX + 2048];
static int fifo_n;
static int audio_ok;

static void audio_init(void)
{
    int i;
    if (R_FAILED(audoutInitialize())) return;
    if (R_FAILED(audoutStartAudioOut())) { audoutExit(); return; }
    for (i = 0; i < ABUF_COUNT; i++) {
        adata[i] = (s16 *)memalign(0x1000, ABUF_BYTES);
        memset(adata[i], 0, ABUF_BYTES);
        memset(&abuf[i], 0, sizeof(abuf[i]));
        abuf[i].buffer      = adata[i];
        abuf[i].buffer_size = ABUF_BYTES;
        abuf[i].data_size   = ABUF_BYTES;
        abuf[i].data_offset = 0;
        free_list[free_n++] = &abuf[i];
    }
    audio_ok = 1;
}

static void audio_pump(void)
{
    s16 tmp[2048];
    size_t got = nes_get_audio(tmp, 2048);
    AudioOutBuffer *rel = NULL;
    u32 cnt = 0;

    if (!audio_ok) return;

    /* recycle buffers the console finished playing (they finish in order) */
    while (R_SUCCEEDED(audoutGetReleasedAudioOutBuffer(&rel, &cnt)) && cnt > 0) {
        u32 k;
        for (k = 0; k < cnt && sub_n > 0; k++) {
            free_list[free_n++] = sub_q[sub_head];
            sub_head = (sub_head + 1) % ABUF_COUNT;
            sub_n--;
        }
        cnt = 0;
    }

    if (fifo_n + (int)got > FIFO_MAX) got = (size_t)(FIFO_MAX - fifo_n); /* keep latency low */
    memcpy(&fifo[fifo_n], tmp, got * sizeof(s16));
    fifo_n += (int)got;

    while (fifo_n >= ABUF_FRAMES && free_n > 0) {
        AudioOutBuffer *b = free_list[--free_n];
        s16 *o = (s16 *)b->buffer;
        int i;
        for (i = 0; i < ABUF_FRAMES; i++) o[2 * i] = o[2 * i + 1] = fifo[i];
        b->data_size = ABUF_BYTES;
        audoutAppendAudioOutBuffer(b);
        sub_q[(sub_head + sub_n) % ABUF_COUNT] = b;
        sub_n++;
        memmove(fifo, fifo + ABUF_FRAMES, (fifo_n - ABUF_FRAMES) * sizeof(s16));
        fifo_n -= ABUF_FRAMES;
    }
}

static void audio_exit(void)
{
    if (!audio_ok) return;
    audoutStopAudioOut();
    audoutExit();
}

/* ---------------------------------------------------------------- input */
#define DEADZONE 12000

/* Maps one controller to NES buttons.
 * Works for: handheld, Pro Controller, Joy-Con pair (dual), and a SINGLE
 * Joy-Con held sideways (set up with Home > Controllers > Change Grip/Order). */
static uint8_t map_pad(PadState *p)
{
    u64 k = padGetButtons(p);
    u32 style = padGetStyleSet(p);
    HidAnalogStickState st;
    uint8_t s = 0;
    int x, y;

    if (style & HidNpadStyleTag_NpadJoyLeft) {
        /* left Joy-Con sideways: rail on top, stick on the left */
        st = padGetStickPos(p, 0);
        x = -st.y; y = st.x;
        if (k & (HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right)) s |= NES_A;
        if (k & (HidNpadButton_LeftSL | HidNpadButton_LeftSR)) s |= NES_B;
        if (k & HidNpadButton_Minus) s |= NES_START;
        if (k & (HidNpadButton_L | HidNpadButton_ZL)) s |= NES_SELECT;
    } else if (style & HidNpadStyleTag_NpadJoyRight) {
        /* right Joy-Con sideways: rail on top, stick on the right */
        st = padGetStickPos(p, 1);
        x = st.y; y = -st.x;
        if (k & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_X | HidNpadButton_Y)) s |= NES_A;
        if (k & (HidNpadButton_RightSL | HidNpadButton_RightSR)) s |= NES_B;
        if (k & HidNpadButton_Plus) s |= NES_START;
        if (k & (HidNpadButton_R | HidNpadButton_ZR)) s |= NES_SELECT;
    } else {
        st = padGetStickPos(p, 0);
        x = st.x; y = st.y;
        if (k & (HidNpadButton_A | HidNpadButton_X)) s |= NES_A;
        if (k & (HidNpadButton_B | HidNpadButton_Y)) s |= NES_B;
        if (k & HidNpadButton_Minus) s |= NES_SELECT;
        if (k & HidNpadButton_Plus)  s |= NES_START;
        if (k & HidNpadButton_Up)    s |= NES_UP;
        if (k & HidNpadButton_Down)  s |= NES_DOWN;
        if (k & HidNpadButton_Left)  s |= NES_LEFT;
        if (k & HidNpadButton_Right) s |= NES_RIGHT;
    }
    if (y >  DEADZONE) s |= NES_UP;
    if (y < -DEADZONE) s |= NES_DOWN;
    if (x < -DEADZONE) s |= NES_LEFT;
    if (x >  DEADZONE) s |= NES_RIGHT;
    return s;
}

int main(int argc, char **argv)
{
    Framebuffer fb;
    PadState pad1, pad2;
    NWindow *win;
    u32 line[OUT_W];
    int frame = 0;

    padConfigureInput(2, HidNpadStyleSet_NpadStandard);
    hidSetNpadJoyHoldType(HidNpadJoyHoldType_Vertical); /* raw Joy-Con input; sideways is handled in map_pad() */
    padInitialize(&pad1, HidNpadIdType_No1, HidNpadIdType_Handheld);
    padInitialize(&pad2, HidNpadIdType_No2);

    if (nes_load(rom_data, rom_size) != 0) {
        consoleInit(NULL);
        printf("ROM failed to load.\nPress + to exit.\n");
        while (appletMainLoop()) {
            padUpdate(&pad1);
            if (padGetButtonsDown(&pad1) & HidNpadButton_Plus) break;
            consoleUpdate(NULL);
        }
        consoleExit(NULL);
        return 1;
    }

    audio_init();

    win = nwindowGetDefault();
    framebufferCreate(&fb, win, 1280, 720, PIXEL_FORMAT_RGBA_8888, 2);
    framebufferMakeLinear(&fb);

    while (appletMainLoop()) {
        u32 stride, *out;
        int y, x;
        u64 held;

        padUpdate(&pad1);
        padUpdate(&pad2);
        held = padGetButtons(&pad1) | padGetButtons(&pad2);
        if ((held & HidNpadButton_L) && (held & HidNpadButton_R)) break; /* L+R quits */

        nes_set_pad(map_pad(&pad1));
        nes_set_pad2(map_pad(&pad2));
        nes_run_frame();
        audio_pump();

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

    audio_exit();
    framebufferClose(&fb);
    return 0;
}
