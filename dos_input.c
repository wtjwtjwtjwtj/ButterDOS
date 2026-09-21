/*
 * dos_input.c – INT 9 keyboard hook for full held-key tracking.
 *
 * We own the keyboard: read the scan code, update state, send EOI once,
 * return. We do NOT chain to the BIOS handler, because chaining causes
 * the BIOS to read an already-consumed scan code and to send a second
 * EOI, which corrupts the 8259 PIC and triggers spurious timer IRQs
 * that manifest as a DOS "divide by zero" error.
 *
 * The original INT 9 vector is restored on shutdown so DOS keyboard
 * input (and Ctrl+Alt+Del) work normally after the program exits.
 */

#include "dos_input.h"
#include <dpmi.h>
#include <go32.h>
#include <pc.h>
#include <string.h>

static unsigned char key_state[256];
static unsigned char key_prev [256];
static volatile int  ext_pending = 0;

static _go32_dpmi_seginfo old_info;
static _go32_dpmi_seginfo new_info;

/* -------- ISR -------------------------------------------------------- */

static void kbd_isr(void)
{
    unsigned char sc = inportb(0x60);

    if (sc == 0xE0) {
        ext_pending = 1;
    } else if (sc == 0xE1) {
        /* Pause/Break prefix – ignore. */
    } else {
        unsigned char code     = sc & 0x7F;
        int           released = (sc & 0x80) ? 1 : 0;

        if (ext_pending) {
            code |= 0x80;
            ext_pending = 0;
        }

        key_state[code] = released ? 0 : 1;
    }

    /* End-of-interrupt to the master PIC. */
    outportb(0x20, 0x20);
}

/* -------- Public API ------------------------------------------------- */

int dos_input_init(void)
{
    memset(key_state, 0, sizeof(key_state));
    memset(key_prev,  0, sizeof(key_prev));
    ext_pending = 0;

    _go32_dpmi_lock_data(key_state, sizeof(key_state));
    _go32_dpmi_lock_data(key_prev,  sizeof(key_prev));
    _go32_dpmi_lock_data((void *)&ext_pending, sizeof(ext_pending));
    _go32_dpmi_lock_code((void *)kbd_isr, 0x200);

    /* Save the current INT 9 vector so we can restore it on shutdown. */
    _go32_dpmi_get_protected_mode_interrupt_vector(0x09, &old_info);

    /* Install our own handler (no chaining). */
    new_info.pm_offset   = (unsigned long)kbd_isr;
    new_info.pm_selector = _go32_my_cs();
    if (_go32_dpmi_allocate_iret_wrapper(&new_info) != 0)
        return -1;

    _go32_dpmi_set_protected_mode_interrupt_vector(0x09, &new_info);
    return 0;
}

void dos_input_shutdown(void)
{
    _go32_dpmi_set_protected_mode_interrupt_vector(0x09, &old_info);
    _go32_dpmi_free_iret_wrapper(&new_info);
}

void dos_input_poll(void)
{
    int i;
    for (i = 0; i < 256; i++)
        key_prev[i] = key_state[i];
}

int dos_input_down(int key)
{
    return key_state[(unsigned char)key] ? 1 : 0;
}

int dos_input_pressed(int key)
{
    unsigned char k = (unsigned char)key;
    return (key_state[k] && !key_prev[k]) ? 1 : 0;
}

int dos_input_released(int key)
{
    unsigned char k = (unsigned char)key;
    return (!key_state[k] && key_prev[k]) ? 1 : 0;
}
