/*
 * dos_font.h – Minimal 5x7 bitmap font (digits only) for the FPS overlay.
 */

#ifndef DOS_FONT_H
#define DOS_FONT_H

/* Draws a single digit (0-9) as a 5x7 glyph. Non-digits are ignored. */
void dos_font_digit(int x, int y, int d, unsigned char color);

/* Draws an unsigned integer right-padded to `width` digits. */
void dos_font_uint(int x, int y, unsigned int n, int width, unsigned char color);

#endif
