/*
 * dos_time.h – High-resolution timing for Butterscotch on DOS.
 *
 * Combines the BIOS 18.2 Hz tick counter (INT 1Ah) with the 8253/8254 PIT
 * channel-0 counter at 1.193182 MHz to give ~1 microsecond resolution.
 *
 * All values are relative to the moment dos_time_init() (or dos_time_reset())
 * was last called.
 */

#ifndef DOS_TIME_H
#define DOS_TIME_H

#include <stdint.h>

int      dos_time_init(void);
void     dos_time_shutdown(void);

/* Resets the time origin to "now". Useful after long loading screens. */
void     dos_time_reset(void);

/* Microseconds since init/reset. Wraps at 2^32 us ≈ 71.6 minutes.
 For* longer runs use dos_time_us64(). */
uint32_t dos_time_us(void);

/* Milliseconds since init/reset. Wraps at 2^32 ms ≈ 49.7 days. */
uint32_t dos_time_ms(void);

/* 64-bit microseconds since init/reset. Never wraps in practice. */
uint64_t dos_time_us64(void);

/* Busy-waits (with interrupts on) until `ms` milliseconds have passed. */
void     dos_time_sleep_ms(uint32_t ms);

/* Call once per rendered frame; computes rolling 1-second average FPS. */
void     dos_time_frame_tick(void);
uint32_t dos_time_fps(void);

#endif
