/*
 * dos_input.h – Keyboard input for Butterscotch on DOS.
 * Uses an INT 9 hook so held-key state is tracked continuously.
 */

#ifndef DOS_INPUT_H
#define DOS_INPUT_H

/* Virtual key codes. Normal keys are the BIOS scan code (0x01..0x7F).
 Ext*ended keys have bit 7 set (0x80 | scan code). */
#define DK_ESC      0x01
#define DK_W        0x11
#define DK_A        0x1E
#define DK_S        0x1F
#define DK_D        0x20
#define DK_Z        0x2C
#define DK_X        0x2D
#define DK_C        0x2E
#define DK_LSHIFT   0x2A
#define DK_RSHIFT   0x36
#define DK_LCTRL    0x1D
#define DK_LALT     0x38
#define DK_ENTER    0x1C
#define DK_SPACE    0x39
#define DK_RCTRL    0x9D   /* 0x1D | 0x80 */
#define DK_RALT     0xB8   /* 0x38 | 0x80 */
#define DK_UP       0xC8   /* 0x48 | 0x80 */
#define DK_DOWN     0xD0   /* 0x50 | 0x80 */
#define DK_LEFT     0xCB   /* 0x4B | 0x80 */
#define DK_RIGHT    0xCD   /* 0x4D | 0x80 */
#define DK_F1       0x3B
#define DK_F2       0x3C
#define DK_F3       0x3D
#define DK_F4       0x3E
#define DK_F5       0x3F
#define DK_F6       0x40
#define DK_F7       0x41
#define DK_F8       0x42
#define DK_F9       0x43
#define DK_F10      0x44

int  dos_input_init(void);
void dos_input_shutdown(void);

/* Call once at the top of each frame. Snapshots state for edge detection. */
void dos_input_poll(void);

/* 1 if the key is currently held down. */
int  dos_input_down(int key);

/* 1 if the key was pressed since the last poll. */
int  dos_input_pressed(int key);

/* 1 if the key was released since the last poll. */
int  dos_input_released(int key);

#endif
