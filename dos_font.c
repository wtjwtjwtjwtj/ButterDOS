/*
 * dos_font.c – Minimal 5x7 bitmap font (digits only).
 */

#include "dos_font.h"
#include "dos_video.h"

/* 5x7 glyphs, one byte per row, low 5 bits used (bit 4 = leftmost). */
static const unsigned char digit_glyph[10][7] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },  /* 0 */
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },  /* 1 */
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },  /* 2 */
    { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },  /* 3 */
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },  /* 4 */
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },  /* 5 */
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },  /* 6 */
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },  /* 7 */
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },  /* 8 */
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },  /* 9 */
};

void dos_font_digit(int x, int y, int d, unsigned char color)
{
    int row, col;
    if (d < 0 || d > 9) return;
    for (row = 0; row < 7; row++) {
        unsigned char bits = digit_glyph[d][row];
        for (col = 0; col < 5; col++) {
            if (bits & (1 << (4 - col))) {
                dos_video_pixel(x + col, y + row, color);
            }
        }
    }
}

void dos_font_uint(int x, int y, unsigned int n, int width, unsigned char color)
{
    int i;
    unsigned int divisor = 1;

    if (width <= 0) return;
    for (i = 1; i < width; i++) divisor *= 10;

    for (i = 0; i < width; i++) {
        int d = (n / divisor) % 10;
        dos_font_digit(x + i * 6, y, d, color);
        divisor /= 10;
    }
}
