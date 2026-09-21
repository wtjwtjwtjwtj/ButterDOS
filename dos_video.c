/*
 * dos_video.c – VGA mode 13h + VESA 8bpp banked, with canvas scaling
 * and auto mode selection.
 */

#include "dos_video.h"
#include <dpmi.h>
#include <go32.h>
#include <pc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/nearptr.h>

static unsigned char  *vga_fb        = 0;
static unsigned char  *back_buffer   = 0;
static unsigned char  *scaled_buffer = 0;
static int             nearptr_on    = 0;

static int             cur_w      = 0;
static int             cur_h      = 0;
static int             cur_pitch  = 0;
static int             current_mode = -1;

static int             canvas_w = 0;
static int             canvas_h = 0;

static _go32_dpmi_seginfo mode_seg = { 0 };
static int             cur_bank   = -1;
static int             bank_gran  = 64 * 1024;
static int             window_size = 64 * 1024;

static unsigned short *xmap = 0;
static unsigned short *ymap = 0;

/* Persistent DOS blocks for VBE calls. Kept across the entire program
 *  lifetime so we can make repeated 4F00/4F01 calls during auto-detect
 *  without reallocating. */
static _go32_dpmi_seginfo ctrl_seg = { 0 };
static int                ctrl_allocated = 0;

/* ---- mode 13h ------------------------------------------------------ */

static int setup_mode13h(void)
{
    __dpmi_regs r;

    r.x.ax = 0x0013;
    __dpmi_int(0x10, &r);

    vga_fb      = (unsigned char *)(__djgpp_conventional_base + 0xA0000UL);
    cur_w       = 320;
    cur_h       = 200;
    cur_pitch   = 320;
    bank_gran   = 64 * 1024;
    window_size = 64 * 1024;
    cur_bank    = -1;
    return 0;
}

/* ---- VBE helpers --------------------------------------------------- */

static void free_vesa_blocks(void)
{
    if (ctrl_seg.pm_selector) {
        _go32_dpmi_free_dos_memory(&ctrl_seg);
        ctrl_seg.pm_selector = 0;
        ctrl_allocated = 0;
    }
    if (mode_seg.pm_selector) {
        _go32_dpmi_free_dos_memory(&mode_seg);
        mode_seg.pm_selector = 0;
    }
}

/* Allocate ctrl_seg and mode_seg if not already done. */
static int alloc_vesa_blocks(void)
{
    if (!ctrl_allocated) {
        ctrl_seg.size = 32;
        if (_go32_dpmi_allocate_dos_memory(&ctrl_seg) != 0)
            return -1;
        ctrl_allocated = 1;
    }
    if (!mode_seg.pm_selector) {
        mode_seg.size = 16;
        if (_go32_dpmi_allocate_dos_memory(&mode_seg) != 0)
            return -1;
    }
    return 0;
}

/* Fetch VBE controller info. Returns 0 on success. */
static int vbe_get_controller(unsigned char **out_ctrl)
{
    unsigned long   ctrl_lin;
    unsigned char  *ctrl;
    __dpmi_regs     r;

    if (alloc_vesa_blocks() != 0)
        return -1;

    ctrl_lin = (unsigned long)ctrl_seg.rm_segment * 16UL;
    ctrl = (unsigned char *)(__djgpp_conventional_base + ctrl_lin);

    ctrl[0] = 'V'; ctrl[1] = 'B'; ctrl[2] = 'E'; ctrl[3] = '2';

    r.x.ax = 0x4F00;
    r.x.di = 0;
    r.x.es = ctrl_seg.rm_segment;
    __dpmi_int(0x10, &r);

    if (r.x.ax != 0x004F) return -1;
    if (ctrl[0] != 'V' || ctrl[1] != 'E' ||
        ctrl[2] != 'S' || ctrl[3] != 'A') return -1;

    *out_ctrl = ctrl;
    return 0;
}

/* Fetch mode info for a given mode number. Returns 0 on success. */
static int vbe_get_mode_info(unsigned short mode, unsigned char **out_mode)
{
    unsigned long   mode_lin;
    unsigned char  *mode_ptr;
    __dpmi_regs     r;

    mode_lin = (unsigned long)mode_seg.rm_segment * 16UL;
    mode_ptr = (unsigned char *)(__djgpp_conventional_base + mode_lin);

    r.x.ax = 0x4F01;
    r.x.cx = mode;
    r.x.di = 0;
    r.x.es = mode_seg.rm_segment;
    __dpmi_int(0x10, &r);

    if (r.x.ax != 0x004F) return -1;
    *out_mode = mode_ptr;
    return 0;
}

/* Set the specified VBE mode (no LFB bit; we use banking). */
static int vbe_set_mode(unsigned short mode)
{
    __dpmi_regs r;
    r.x.ax = 0x4F02;
    r.x.bx = mode;
    r.x.di = 0;
    __dpmi_int(0x10, &r);
    return (r.x.ax == 0x004F) ? 0 : -1;
}

/* Populate globals from the currently selected VBE mode. */
static void vbe_apply_mode_info(unsigned short mode)
{
    unsigned char *mode_ptr;

    if (vbe_get_mode_info(mode, &mode_ptr) != 0) {
        /* Fallback — should never happen since we just tested it. */
        bank_gran   = 64 * 1024;
        window_size = 64 * 1024;
        cur_pitch   = 640;
        return;
    }

    {
        unsigned short gran  = *(unsigned short *)(mode_ptr + 0x04);
        unsigned short win   = *(unsigned short *)(mode_ptr + 0x06);
        unsigned short pitch = *(unsigned short *)(mode_ptr + 0x10);
        unsigned short xres  = *(unsigned short *)(mode_ptr + 0x12);
        unsigned short yres  = *(unsigned short *)(mode_ptr + 0x14);

        bank_gran   = gran  ? (int)gran  * 1024 : 64 * 1024;
        window_size = win   ? (int)win   * 1024 : 64 * 1024;
        cur_pitch   = pitch ? (int)pitch         : (int)xres;
        cur_w       = (int)xres;
        cur_h       = (int)yres;
    }

    vga_fb   = (unsigned char *)(__djgpp_conventional_base + 0xA0000UL);
    cur_bank = -1;
}

/* ---- VESA mode enumeration and selection --------------------------- */

typedef struct {
    unsigned short num;
    unsigned short w;
    unsigned short h;
    unsigned char  bpp;
    unsigned char  attr;
} vbe_mode_t;

#define MAX_VBE_MODES 64

static int enumerate_8bpp_modes(vbe_mode_t *out, int max_modes)
{
    unsigned char  *ctrl;
    unsigned short *modes;
    unsigned long   modes_lin;
    unsigned short  modes_off, modes_seg;
    int             count = 0;
    int             i;

    if (vbe_get_controller(&ctrl) != 0)
        return 0;

    modes_off = *(unsigned short *)(ctrl + 14);
    modes_seg = *(unsigned short *)(ctrl + 16);
    modes_lin = (unsigned long)modes_seg * 16UL + (unsigned long)modes_off;
    modes = (unsigned short *)(__djgpp_conventional_base + modes_lin);

    for (i = 0; i < 1024 && count < max_modes; i++) {
        unsigned short m = modes[i];
        unsigned char *mode_ptr;
        unsigned short xres, yres;
        unsigned char  attr, bpp;

        if (m == 0xFFFF) break;

        if (vbe_get_mode_info(m, &mode_ptr) != 0) continue;

        attr = mode_ptr[0x00];
        xres = *(unsigned short *)(mode_ptr + 0x12);
        yres = *(unsigned short *)(mode_ptr + 0x14);
        bpp  = mode_ptr[0x19];

        /* Only 8bpp color modes that are currently available. */
        if (bpp != 8)             continue;
        if (!(attr & 0x01))       continue;   /* not supported  */
            if (!(attr & 0x08))       continue;   /* not color      */
                if (xres < 320 || yres < 200) continue;
                if (xres > 1024 || yres > 768) continue;   /* cap memory use */

                    out[count].num  = m;
        out[count].w    = xres;
        out[count].h    = yres;
        out[count].bpp  = bpp;
        out[count].attr = attr;
        count++;
    }

    return count;
}

/* Find the best mode given a target. Returns 0 if none found. */
static unsigned short pick_best_mode(int preferred_w, int preferred_h,
                                     vbe_mode_t *modes, int count)
{
    int           best_exact       = -1;
    int           best_ge          = -1;
    int           best_ge_score    = 0x7FFFFFFF;
    int           best_largest     = -1;
    int           best_largest_px  = 0;
    int           i;

    for (i = 0; i < count; i++) {
        int w = modes[i].w;
        int h = modes[i].h;
        int px = w * h;

        /* Exact match. */
        if (w == preferred_w && h == preferred_h) {
            best_exact = i;
            break;
        }

        /* Smallest mode that is >= preferred in both dimensions. */
        if (preferred_w > 0 && preferred_h > 0 &&
            w >= preferred_w && h >= preferred_h) {
            int score = px - preferred_w * preferred_h;
        if (score < best_ge_score) {
            best_ge_score = score;
            best_ge = i;
        }
            }

            /* Overall largest, for when there is no preference. */
            if (px > best_largest_px) {
                best_largest_px = px;
                best_largest = i;
            }
    }

    if (best_exact >= 0)   return modes[best_exact].num;
    if (best_ge >= 0)      return modes[best_ge].num;
    if (best_largest >= 0) return modes[best_largest].num;
    return 0;
}

/* ---- scale maps ---------------------------------------------------- */

static void free_scale_maps(void)
{
    if (xmap) { free(xmap); xmap = 0; }
    if (ymap) { free(ymap); ymap = 0; }
}

static int build_scale_maps(void)
{
    int i;

    free_scale_maps();

    xmap = (unsigned short *)malloc((size_t)cur_w * sizeof(unsigned short));
    ymap = (unsigned short *)malloc((size_t)cur_h * sizeof(unsigned short));
    if (!xmap || !ymap) { free_scale_maps(); return -1; }

    for (i = 0; i < cur_w; i++)
        xmap[i] = (unsigned short)((unsigned long)i * (unsigned long)canvas_w
        / (unsigned long)cur_w);
    for (i = 0; i < cur_h; i++)
        ymap[i] = (unsigned short)((unsigned long)i * (unsigned long)canvas_h
        / (unsigned long)cur_h);
    return 0;
}

/* ---- public API ---------------------------------------------------- */

int dos_video_init(int mode)
{
    if (__djgpp_nearptr_enable() == 0)
        return -1;
    nearptr_on = 1;

    if (mode == DOS_VIDEO_MODE13H) {
        if (setup_mode13h() != 0) {
            __djgpp_nearptr_disable();
            nearptr_on = 0;
            return -1;
        }
    } else if (mode == DOS_VIDEO_MODE_VESA640) {
        unsigned char *mode_ptr;

        if (vbe_get_controller(&mode_ptr) != 0 ||
            vbe_set_mode(0x0101) != 0) {   /* 0x0101 is the standard 640x480x8 */
                free_vesa_blocks();
                __djgpp_nearptr_disable();
                nearptr_on = 0;
                return -1;
            }
            vbe_apply_mode_info(0x0101);
    } else {
        __djgpp_nearptr_disable();
        nearptr_on = 0;
        return -1;
    }

    canvas_w = cur_w;
    canvas_h = cur_h;

    back_buffer = (unsigned char *)malloc((size_t)canvas_w * (size_t)canvas_h);
    if (!back_buffer) { dos_video_shutdown(); return -1; }
    memset(back_buffer, 0, (size_t)canvas_w * (size_t)canvas_h);

    current_mode = mode;
    return 0;
}

int dos_video_init_best(int preferred_w, int preferred_h)
{
    vbe_mode_t     modes[MAX_VBE_MODES];
    unsigned short chosen;
    int            count;

    if (__djgpp_nearptr_enable() == 0)
        return -1;
    nearptr_on = 1;

    count = enumerate_8bpp_modes(modes, MAX_VBE_MODES);
    if (count > 0) {
        chosen = pick_best_mode(preferred_w, preferred_h, modes, count);
        if (chosen != 0 && vbe_set_mode(chosen) == 0) {
            vbe_apply_mode_info(chosen);

            canvas_w = cur_w;
            canvas_h = cur_h;

            back_buffer = (unsigned char *)malloc((size_t)canvas_w *
            (size_t)canvas_h);
            if (!back_buffer) { dos_video_shutdown(); return -1; }
            memset(back_buffer, 0, (size_t)canvas_w * (size_t)canvas_h);

            current_mode = DOS_VIDEO_MODE_AUTO;
            return 0;
        }
    }

    /* No suitable VESA mode — fall back to mode 13h. */
    if (setup_mode13h() != 0) {
        __djgpp_nearptr_disable();
        nearptr_on = 0;
        return -1;
    }

    canvas_w = cur_w;
    canvas_h = cur_h;

    back_buffer = (unsigned char *)malloc((size_t)canvas_w * (size_t)canvas_h);
    if (!back_buffer) { dos_video_shutdown(); return -1; }
    memset(back_buffer, 0, (size_t)canvas_w * (size_t)canvas_h);

    current_mode = DOS_VIDEO_MODE13H;
    return 0;
}

void dos_video_shutdown(void)
{
    __dpmi_regs r;

    if (back_buffer)   { free(back_buffer);   back_buffer = 0; }
    if (scaled_buffer) { free(scaled_buffer); scaled_buffer = 0; }
    free_scale_maps();

    free_vesa_blocks();
    vga_fb = 0;

    if (nearptr_on) {
        __djgpp_nearptr_disable();
        nearptr_on = 0;
    }

    r.x.ax = 0x0003;
    __dpmi_int(0x10, &r);

    cur_w = cur_h = cur_pitch = 0;
    canvas_w = canvas_h = 0;
    current_mode = -1;
    cur_bank = -1;
}

int dos_video_width(void)         { return cur_w; }
int dos_video_height(void)        { return cur_h; }
int dos_video_canvas_width(void)  { return canvas_w; }
int dos_video_canvas_height(void) { return canvas_h; }

int dos_video_set_canvas(int w, int h)
{
    if (w <= 0 || h <= 0) return -1;

    if (back_buffer)   { free(back_buffer);   back_buffer = 0; }
    if (scaled_buffer) { free(scaled_buffer); scaled_buffer = 0; }
    free_scale_maps();

    canvas_w = w;
    canvas_h = h;

    back_buffer = (unsigned char *)malloc((size_t)w * (size_t)h);
    if (!back_buffer) return -1;
    memset(back_buffer, 0, (size_t)w * (size_t)h);

    if (w != cur_w || h != cur_h) {
        scaled_buffer = (unsigned char *)malloc((size_t)cur_pitch *
        (size_t)cur_h);
        if (!scaled_buffer) {
            free(back_buffer); back_buffer = 0;
            return -1;
        }
        if (build_scale_maps() != 0) {
            free(scaled_buffer); scaled_buffer = 0;
            free(back_buffer);   back_buffer = 0;
            return -1;
        }
    }
    return 0;
}

void dos_video_clear(unsigned char color)
{
    if (!back_buffer) return;
    memset(back_buffer, color, (size_t)canvas_w * (size_t)canvas_h);
}

void dos_video_pixel(int x, int y, unsigned char color)
{
    if (!back_buffer) return;
    if ((unsigned)x >= (unsigned)canvas_w || (unsigned)y >= (unsigned)canvas_h)
        return;
    back_buffer[(size_t)y * (size_t)canvas_w + x] = color;
}

void dos_video_rect(int x, int y, int w, int h, unsigned char color)
{
    int x0, y0, x1, y1, row;

    if (!back_buffer || w <= 0 || h <= 0) return;

    x0 = x; y0 = y;
    x1 = x + w; y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > canvas_w) x1 = canvas_w;
    if (y1 > canvas_h) y1 = canvas_h;

    for (row = y0; row < y1; row++)
        memset(back_buffer + (size_t)row * (size_t)canvas_w + x0,
               color, (size_t)(x1 - x0));
}

void dos_video_palette_set(int idx, unsigned char r,
                           unsigned char g,
                           unsigned char b)
{
    outportb(0x3C8, (unsigned char)(idx & 0xFF));
    outportb(0x3C9, r & 0x3F);
    outportb(0x3C9, g & 0x3F);
    outportb(0x3C9, b & 0x3F);
}

static void vesa_set_bank(int bank)
{
    __dpmi_regs r;
    if (bank == cur_bank) return;
    r.x.ax = 0x4F05;
    r.x.bx = 0;
    r.x.dx = (unsigned short)bank;
    __dpmi_int(0x10, &r);
    cur_bank = bank;
}

static void blit_to_display(unsigned long off, const unsigned char *src,
                            unsigned long n)
{
    if (current_mode == DOS_VIDEO_MODE13H) {
        memcpy(vga_fb + off, src, n);
        return;
    }

    {
        unsigned long done = 0;
        while (done < n) {
            unsigned long total_off = off + done;
            unsigned long win_off   = total_off % (unsigned long)window_size;
            unsigned long win_lin   = total_off - win_off;
            int           bank      = (int)(win_lin / (unsigned long)bank_gran);
            unsigned long bank_off  = total_off - (unsigned long)bank *
            (unsigned long)bank_gran;
            unsigned long to_win    = (unsigned long)window_size - win_off;
            unsigned long to_bank   = (unsigned long)bank_gran   - bank_off;
            unsigned long chunk     = to_win < to_bank ? to_win : to_bank;

            if (chunk > n - done) chunk = n - done;

            vesa_set_bank(bank);
            memcpy(vga_fb + bank_off, src + done, chunk);
            done += chunk;
        }
    }
}

void dos_video_present(void)
{
    int dy;

    if (!vga_fb || !back_buffer) return;

    if (current_mode == DOS_VIDEO_MODE13H) {
        while (inportb(0x3DA) & 0x08) { }
        while (!(inportb(0x3DA) & 0x08)) { }
    }

    if (canvas_w == cur_w && canvas_h == cur_h && cur_pitch == cur_w) {
        blit_to_display(0, back_buffer,
                        (unsigned long)cur_w * (unsigned long)cur_h);
        return;
    }

    if (!scaled_buffer || !xmap || !ymap) return;

    for (dy = 0; dy < cur_h; dy++) {
        int sy = ymap[dy];
        const unsigned char *src_row = back_buffer +
        (size_t)sy * (size_t)canvas_w;
        unsigned char       *dst_row = scaled_buffer +
        (size_t)dy * (size_t)cur_pitch;
        int dx;

        for (dx = 0; dx < cur_w; dx++) {
            dst_row[dx] = src_row[xmap[dx]];
        }
    }

    blit_to_display(0, scaled_buffer,
                    (unsigned long)cur_pitch * (unsigned long)cur_h);
}
