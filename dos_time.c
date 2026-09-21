/*
 * dos_time.c – High-resolution monotonic timing for Butterscotch on DOS.
 *
 * Uses DJGPP's uclock(), which reads the 8253/8254 PIT channel-0 counter
 * via a DPMI-safe path and returns a strictly-increasing 64-bit tick
 * count at 1193182 Hz. Earlier versions of this file combined the BIOS
 * tick counter (INT 1Ah) with a direct PIT latch read; under HDPMI32I
 * that combination produced a non-monotonic source that wrapped back to
 * zero once per BIOS tick (every ~54.9 ms), which corrupted both the
 * FPS calculation and (subtly) the delta-time used for motion.
 */

#include "dos_time.h"
#include <time.h>
#include <stdint.h>

#define UCLOCK_HZ 1193182ull

static uclock_t time_origin;
static uclock_t fps_last;
static uint32_t fps_frames;
static uint32_t fps_value;

int dos_time_init(void)
{
    time_origin = uclock();
    fps_last    = time_origin;
    fps_frames  = 0;
    fps_value   = 0;
    return 0;
}

void dos_time_shutdown(void)
{
    /* Nothing to release. */
}

void dos_time_reset(void)
{
    time_origin = uclock();
}

uint64_t dos_time_us64(void)
{
    uclock_t delta = uclock() - time_origin;
    if (delta < 0) delta = 0;   /* defensive: never go backwards */
        return (uint64_t)delta * 1000000ull / UCLOCK_HZ;
}

uint32_t dos_time_us(void)
{
    return (uint32_t)dos_time_us64();
}

uint32_t dos_time_ms(void)
{
    return (uint32_t)(dos_time_us64() / 1000ull);
}

void dos_time_sleep_ms(uint32_t ms)
{
    uclock_t target = uclock() + (uclock_t)ms * UCLOCK_HZ / 1000ull;
    while (uclock() < target) { }
}

void dos_time_frame_tick(void)
{
    uclock_t now     = uclock();
    uclock_t elapsed = now - fps_last;

    fps_frames++;

    /* Safety guard: if the clock jumped more than 10 seconds, assume
     *      something went wrong and reset the window without computing. */
    if (elapsed < 0 || elapsed > (uclock_t)UCLOCK_HZ * 10) {
        fps_last   = now;
        fps_frames = 0;
        fps_value  = 0;
        return;
    }

    if (elapsed >= (uclock_t)UCLOCK_HZ) {   /* 1-second window */
        fps_value = (uint32_t)((uint64_t)fps_frames * UCLOCK_HZ
        / (uint64_t)elapsed);
        fps_frames = 0;
        fps_last   = now;
    }
}

uint32_t dos_time_fps(void)
{
    return fps_value;
}
