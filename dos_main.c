/*
 * dos_main.c – Butterscotch DOS game-loop + auto video mode + timing.
 *
 * Controls:
 *   Arrow keys or WASD  – move the box
 *   ESC                 – quit (then file I/O test runs)
 *
 * Display resolution is selected automatically: we ask for a target
 * (PREFERRED_W x PREFERRED_H) and fall back to the next-larger mode,
 * then to any 8bpp VESA mode, then to mode 13h.
 *
 * The game always renders into a fixed canvas (CANVAS_W x CANVAS_H)
 * that is scaled up to the chosen display resolution.
 */

#include "dos_video.h"
#include "dos_input.h"
#include "dos_file.h"
#include "dos_time.h"
#include "dos_font.h"

#include <stdio.h>
#include <stdlib.h>

/* Desired display resolution. Auto-selection will pick this exactly if
 *  available, otherwise the smallest mode that is >= this in both
 *  dimensions. Set either to 0 to accept any mode. */
#define PREFERRED_W 640
#define PREFERRED_H 480

/* Internal render resolution. Butterscotch-class games render here. */
#define CANVAS_W 320
#define CANVAS_H 200

int main(void)
{
    int i, x, y;
    int running = 1;
    int cw, ch;
    uint64_t last_us;
    const float speed_px_per_sec = 60.0f;

    if (dos_video_init_best(PREFERRED_W, PREFERRED_H) != 0) {
        printf("Video init failed. Aborting.\n");
        return 1;
    }

    printf("Display: %dx%d   Canvas: %dx%d\n",
           dos_video_width(), dos_video_height(),
           CANVAS_W, CANVAS_H);
    fflush(stdout);

    if (dos_video_set_canvas(CANVAS_W, CANVAS_H) != 0) {
        printf("Canvas set failed.\n");
        dos_video_shutdown();
        return 1;
    }

    dos_input_init();
    dos_time_init();

    cw = dos_video_canvas_width();
    ch = dos_video_canvas_height();

    for (i = 0; i < 256; i++) {
        unsigned char r = (i >> 5) & 0x07;
        unsigned char g = (i >> 2) & 0x07;
        unsigned char b = (i     ) & 0x03;
        dos_video_palette_set(i, r * 9, g * 9, b * 21);
    }
    dos_video_palette_set(15, 63, 63, 63);
    dos_video_palette_set(0,   0,  0,  0);

    x = cw / 2 - 8;
    y = ch / 2 - 8;
    last_us = dos_time_us64();

    while (running) {
        uint64_t now_us = dos_time_us64();
        uint64_t dt_us  = now_us - last_us;
        float    dt_s   = (float)dt_us / 1000000.0f;
        int      step   = (int)(speed_px_per_sec * dt_s + 0.5f);

        last_us = now_us;
        if (step < 1) step = 1;

        dos_input_poll();

        if (dos_input_pressed(DK_ESC))
            running = 0;

        if (dos_input_down(DK_LEFT)  || dos_input_down(DK_A)) x -= step;
        if (dos_input_down(DK_RIGHT) || dos_input_down(DK_D)) x += step;
        if (dos_input_down(DK_UP)    || dos_input_down(DK_W)) y -= step;
        if (dos_input_down(DK_DOWN)  || dos_input_down(DK_S)) y += step;

        if (x < 0)       x = 0;
        if (x > cw - 16) x = cw - 16;
        if (y < 0)       y = 0;
        if (y > ch - 16) y = ch - 16;

        dos_video_clear(0x10);
        for (i = 0; i < ch; i += 16) {
            dos_video_rect(0, i, cw, 1, 0x18);
        }
        dos_video_rect(x, y, 16, 16, 15);

        dos_time_frame_tick();
        dos_video_rect(2, 2, 20, 11, 0);
        dos_font_uint(3, 3, dos_time_fps(), 3, 15);

        dos_video_present();
    }

    dos_input_shutdown();

    {
        size_t   sz = 0;
        void    *data;

        dos_video_shutdown();

        printf("\n--- File I/O test ---\n");

        if (dos_file_exists("AUTOEXEC.BAT")) {
            printf("AUTOEXEC.BAT exists, size = %ld\n",
                   dos_file_size("AUTOEXEC.BAT"));

            data = dos_file_read_all("AUTOEXEC.BAT", &sz);
            if (data) {
                printf("Read %u bytes. First 200 chars:\n", (unsigned)sz);
                fwrite(data, 1, sz < 200 ? sz : 200, stdout);
                printf("\n");
                free(data);
            } else {
                printf("dos_file_read_all failed.\n");
            }
        } else {
            printf("AUTOEXEC.BAT not found in current directory.\n");
        }

        printf("\nPress Enter to exit.\n");
        getchar();
    }

    return 0;
}
