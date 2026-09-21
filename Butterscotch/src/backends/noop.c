/*
 * noop.c – DOS platform backend (FreeDOS + HDPMI32I).
 *
 * Hooks INT 9 directly for full make/break scancode access. Gives real
 * key-down AND key-up events, which INT 16h polling cannot provide.
 *
 * The handler does NOT chain to BIOS. This disables Ctrl+Alt+Del while
 * the game runs (as is standard for DOS games); power-cycle to recover.
 * The original vector is restored on clean shutdown.
 */

#include "common.h"
#include "platformdefs.h"
#include "gettime.h"
#include "runner_mouse.h"
#include "runner_keyboard.h"

#include <string.h>
#include <time.h>

#ifdef __DJGPP__
#include <dpmi.h>
#include <go32.h>
#include <pc.h>
#endif

int g_dos_fps_enabled = 0;

/* Defined in dos_audio_system.c */
extern void DosAudioSystem_platformTick(float dt);

static Runner *g_runner = NULL;
static int32_t g_width = 0;
static int32_t g_height = 0;
static bool g_initialized = false;

#ifdef __DJGPP__

/* key_state[code] where code is 0..127 for normal, 128..255 for ext. */
static volatile unsigned char g_key_state[256];
static volatile unsigned char g_ext_pending = 0;

static _go32_dpmi_seginfo g_old_int9;
static _go32_dpmi_seginfo g_new_int9;
static int g_int9_installed = 0;

/* ISR. Do not call any library function here. Only inb/outb inline. */
static void kbd_isr(void)
{
    unsigned char sc;
    __asm__ __volatile__("inb $0x60, %0" : "=a"(sc));

    if (sc == 0xE0) {
        g_ext_pending = 1;
    } else if (sc == 0xE1) {
        /* Pause/Break prefix - ignore. */
    } else {
        unsigned char code     = sc & 0x7F;
        int           released = (sc & 0x80) ? 1 : 0;
        unsigned char idx      = code;

        if (g_ext_pending) {
            idx |= 0x80;
            g_ext_pending = 0;
        }
        g_key_state[idx] = released ? 0 : 1;

        /* F12 toggles the FPS overlay - edge detect on make only. */
        if (!released && idx == 0x58)
            g_dos_fps_enabled = !g_dos_fps_enabled;
    }

    __asm__ __volatile__("movb $0x20, %%al\n\t"
                         "outb %%al, $0x20"
                         : : : "eax");
}

static int install_int9(void)
{
    _go32_dpmi_lock_data((void *)g_key_state, sizeof(g_key_state));
    _go32_dpmi_lock_data((void *)&g_ext_pending, sizeof(g_ext_pending));
    _go32_dpmi_lock_code((void *)kbd_isr, 0x200);

    _go32_dpmi_get_protected_mode_interrupt_vector(0x09, &g_old_int9);

    g_new_int9.pm_offset  = (unsigned long)kbd_isr;
    g_new_int9.pm_selector = _go32_my_cs();
    if (_go32_dpmi_allocate_iret_wrapper(&g_new_int9) != 0)
        return -1;
    _go32_dpmi_set_protected_mode_interrupt_vector(0x09, &g_new_int9);
    g_int9_installed = 1;
    return 0;
}

static void uninstall_int9(void)
{
    if (!g_int9_installed) return;
    _go32_dpmi_set_protected_mode_interrupt_vector(0x09, &g_old_int9);
    _go32_dpmi_free_iret_wrapper(&g_new_int9);
    g_int9_installed = 0;
}

static int scancode_to_gml(int sc, int ext)
{
    if (ext) {
        switch (sc) {
            case 0x48: return VK_UP;
            case 0x50: return VK_DOWN;
            case 0x4B: return VK_LEFT;
            case 0x4D: return VK_RIGHT;
            case 0x47: return VK_HOME;
            case 0x4F: return VK_END;
            case 0x49: return VK_PAGEUP;
            case 0x51: return VK_PAGEDOWN;
            case 0x52: return VK_INSERT;
            case 0x53: return VK_DELETE;
            case 0x1D: return VK_CONTROL;
            case 0x38: return VK_ALT;
            default:   return -1;
        }
    }
    switch (sc) {
        case 0x01: return VK_ESCAPE;
        case 0x0E: return VK_BACKSPACE;
        case 0x0F: return VK_TAB;
        case 0x1C: return VK_ENTER;
        case 0x1D: return VK_CONTROL;
        case 0x2A: return VK_SHIFT;
        case 0x36: return VK_SHIFT;
        case 0x38: return VK_ALT;
        case 0x39: return VK_SPACE;
        case 0x3B: return VK_F1;  case 0x3C: return VK_F2;
        case 0x3D: return VK_F3;  case 0x3E: return VK_F4;
        case 0x3F: return VK_F5;  case 0x40: return VK_F6;
        case 0x41: return VK_F7;  case 0x42: return VK_F8;
        case 0x43: return VK_F9;  case 0x44: return VK_F10;
        case 0x57: return VK_F11; case 0x58: return VK_F12;

        case 0x10: return 'Q';  case 0x11: return 'W';
        case 0x12: return 'E';  case 0x13: return 'R';
        case 0x14: return 'T';  case 0x15: return 'Y';
        case 0x16: return 'U';  case 0x17: return 'I';
        case 0x18: return 'O';  case 0x19: return 'P';
        case 0x1E: return 'A';  case 0x1F: return 'S';
        case 0x20: return 'D';  case 0x21: return 'F';
        case 0x22: return 'G';  case 0x23: return 'H';
        case 0x24: return 'J';  case 0x25: return 'K';
        case 0x26: return 'L';
        case 0x2C: return 'Z';  case 0x2D: return 'X';
        case 0x2E: return 'C';  case 0x2F: return 'V';
        case 0x30: return 'B';  case 0x31: return 'N';
        case 0x32: return 'M';

        case 0x02: return '1';  case 0x03: return '2';
        case 0x04: return '3';  case 0x05: return '4';
        case 0x06: return '5';  case 0x07: return '6';
        case 0x08: return '7';  case 0x09: return '8';
        case 0x0A: return '9';  case 0x0B: return '0';

        default:   return -1;
    }
}

static unsigned char g_key_prev[256];

static void pump_keyboard_events(void)
{
    int i;
    if (!g_runner || !g_runner->keyboard) {
        for (i = 0; i < 256; i++) g_key_prev[i] = g_key_state[i];
        return;
    }

    for (i = 0; i < 256; i++) {
        unsigned char now  = g_key_state[i];
        unsigned char prev = g_key_prev[i];
        int sc, ext, gml;

        if (now == prev) continue;
        g_key_prev[i] = now;

        sc  = i & 0x7F;
        ext = (i & 0x80) ? 1 : 0;
        gml = scancode_to_gml(sc, ext);
        if (gml < 0) continue;

        if (now) {
            RunnerKeyboard_onKeyDown(g_runner->keyboard, gml);
            RunnerKeyboard_onKeyDown(g_runner->keyboard, VK_ANYKEY);
        } else {
            RunnerKeyboard_onKeyUp(g_runner->keyboard, gml);
        }
    }
}

#endif

bool platformInit(int32_t reqW, int32_t reqH, const char *title, bool headless) {
    (void)title; (void)headless;
    g_width  = reqW > 0 ? reqW : 640;
    g_height = reqH > 0 ? reqH : 480;
    g_initialized = true;
#ifdef __DJGPP__
    freopen("STDOUT.TXT", "w", stdout);
    freopen("STDERR.TXT", "w", stderr);
    logInfo("DOS platform backend: %dx%d, INT 9 input\n", g_width, g_height);
    if (install_int9() != 0)
        logInfo("Warning: INT 9 hook install failed\n");
#else
    logInfo("No-op platform backend: %dx%d\n", g_width, g_height);
#endif
    return true;
}

void platformExit(void) {
#ifdef __DJGPP__
    uninstall_int9();
#endif
    g_initialized = false;
}

void platformInitFunctions(Runner *runner) {
    g_runner = runner;
    runner->setCursor = NULL;
    runner->currentCursor = GML_CR_DEFAULT;
}

bool platformGetWindowSize(int32_t *outW, int32_t *outH) {
    if (!outW || !outH || !g_initialized) return false;
    *outW = g_width; *outH = g_height; return true;
}
bool platformGetScaledWindowSize(int32_t *outW, int32_t *outH) {
    return platformGetWindowSize(outW, outH);
}
void platformSetWindowSize(int32_t width, int32_t height) {
    if (width  > 0) g_width  = width;
    if (height > 0) g_height = height;
}
void platformSetWindowTitle(const char *title) { (void)title; }
void platformGetMousePos(double *xPos, double *yPos) {
    if (xPos) *xPos = 0.0; if (yPos) *yPos = 0.0;
}
void platformSwapBuffers(void) { }

void *platformGetProcAddress(const char *name) { (void)name; return NULL; }

bool platformHandleEvents(void) {
#ifdef __DJGPP__
    static uint64_t last_ns = 0;
    uint64_t now;
    float    dt;

    pump_keyboard_events();

    now = nowNanos();
    if (last_ns == 0) {
        dt = 0.033f;
    } else {
        dt = (float)(now - last_ns) / 1000000000.0f;
        if (dt < 0.0f)  dt = 0.0f;
        if (dt > 0.1f)  dt = 0.1f;
    }
    last_ns = now;

    DosAudioSystem_platformTick(dt);
#endif
    return false;
}

void platformSleepUntil(uint64_t time) {
    int64_t remaining = (int64_t)time - (int64_t)nowNanos();
    if (remaining > 2000000) {
        remaining -= 1000000;
#if defined(_WIN32)
        Sleep(remaining / 1000000);
#elif defined(__DJGPP__)
        (void)remaining;
#else
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = remaining;
        nanosleep(&ts, NULL);
#endif
    }
    while (nowNanos() < time) {
        YIELD();
    }
}
