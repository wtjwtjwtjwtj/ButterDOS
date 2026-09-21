/*
 * dos_video.h – Graphics backend for Butterscotch on DOS.
 *
 * Modes:
 *   DOS_VIDEO_MODE13H      – VGA 320x200, fixed
 *   DOS_VIDEO_MODE_VESA640 – VESA 640x480x8, fixed
 *   DOS_VIDEO_MODE_AUTO    – auto-select best available
 *
 * dos_video_init() takes a mode constant. For AUTO, pass the target
 * resolution through dos_video_init_best() instead — that lets you
 * say "I want 800x600 if you have it, otherwise the closest thing".
 */

#ifndef DOS_VIDEO_H
#define DOS_VIDEO_H

#define DOS_VIDEO_MODE13H       0
#define DOS_VIDEO_MODE_VESA640  1
#define DOS_VIDEO_MODE_AUTO     2

/* Explicit mode selection (13h or VESA640). */
int  dos_video_init(int mode);

/* Auto-select the best VESA 8bpp mode.
 p re*ferred_w/preferred_h: desired size. Pass 0,0 for "give me the
 largest reasonable mode". Selection order:
 1. exact match for (preferred_w, preferred_h)
 2. smallest mode >= preferred in both dimensions
 3. largest mode with width <= 1024 (to cap memory use)
 4. mode 13h if no VESA 8bpp mode is available
 Returns 0 on success. On success, dos_video_width/height report the
 actual chosen resolution. */
int  dos_video_init_best(int preferred_w, int preferred_h);

void dos_video_shutdown(void);

int  dos_video_width(void);
int  dos_video_height(void);
int  dos_video_canvas_width(void);
int  dos_video_canvas_height(void);

int  dos_video_set_canvas(int w, int h);

void dos_video_clear(unsigned char color);
void dos_video_pixel(int x, int y, unsigned char color);
void dos_video_rect(int x, int y, int w, int h, unsigned char color);

void dos_video_palette_set(int idx, unsigned char r,
                           unsigned char g,
                           unsigned char b);

void dos_video_present(void);

#endif
