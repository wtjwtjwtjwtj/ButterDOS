/*
 * dos_renderer.c – Software renderer for DOS (VESA 640x480x8, banked).
 *
 * Palette: fixed 6-6-6 web-safe RGB cube (216 entries) + 40 grays.
 * Color masking: full multiply, GameMaker semantics (colors are little-endian
 *   RGB: 0x00BBGGRR, so R = low byte, B = high byte).
 * Alpha: real per-pixel blend against the existing framebuffer.
 */

#include "dos_renderer.h"
#include "common.h"
#include "renderer.h"
#include "runner.h"
#include "data_win.h"
#include "image_decoder.h"
#include "utils.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <dpmi.h>
#include <go32.h>
#include <pc.h>
#include <sys/nearptr.h>

#define SCREEN_W       640
#define SCREEN_H       480
#define VGA_WINDOW     0xA0000UL
#define VESA_MODE_640  0x0101

extern int g_dos_fps_enabled;

/* ---- Module state --------------------------------------------------- */

static uint8_t           *g_fb          = NULL;
static uint8_t           *g_vga         = NULL;
static int                g_bank_gran   = 64 * 1024;
static int                g_window_size = 64 * 1024;
static int                g_cur_bank    = -1;
static int                g_nearptr     = 0;
static _go32_dpmi_seginfo g_mode_seg    = { 0 };
static int                g_mode_alloc  = 0;

static int32_t g_view_x = 0, g_view_y = 0;
static int32_t g_view_w = 0, g_view_h = 0;
static int32_t g_port_x = 0, g_port_y = 0;
static int32_t g_port_w = 0, g_port_h = 0;
static float   g_view_scale_x = 1.0f;
static float   g_view_scale_y = 1.0f;
static int     g_in_view = 0;

typedef struct {
    uint8_t *rgba;
    int      w;
    int      h;
    bool     loaded;
    uint64_t last_use;
} TexPage;
static TexPage  *g_tex       = NULL;
static uint32_t  g_tex_count = 0;
static uint64_t  g_tex_tick  = 0;

static uint32_t g_fps_value   = 0;
static uint32_t g_fps_frames  = 0;
static uint64_t g_fps_last_ns = 0;

/* ---- DOSRenderer struct --------------------------------------------- */

typedef struct {
    Renderer base;
    int32_t *surfaceWidths;
    int32_t *surfaceHeights;
    bool    *surfaceExistsFlag;
    uint32_t surfaceCount;
    uint32_t surfaceCapacity;
    bool blendEnable;
    int32_t blendMode;
    BlendFactors blendFactors;
    bool alphaTestEnable;
    uint8_t alphaTestRef;
    bool colorWriteR, colorWriteG, colorWriteB, colorWriteA;
    bool fogEnable;
    uint32_t fogColor;
} DOSRenderer;

/* ---- Palette -------------------------------------------------------- */

/* 8-bit RGB reconstruction of each palette entry, for alpha blending. */
static uint8_t g_pal_lookup[256][3];

static void pal_set(int idx, uint8_t r6, uint8_t g6, uint8_t b6)
{
    outportb(0x3C8, (uint8_t)(idx & 0xFF));
    outportb(0x3C9, r6 & 0x3F);
    outportb(0x3C9, g6 & 0x3F);
    outportb(0x3C9, b6 & 0x3F);
}

static void pal_install(void)
{
    int i;
    for (i = 0; i < 216; i++) {
        int r6 = (i / 36) * 63 / 5;
        int g6 = ((i / 6) % 6) * 63 / 5;
        int b6 = (i % 6) * 63 / 5;
        pal_set(i, r6, g6, b6);
        g_pal_lookup[i][0] = (uint8_t)((r6 << 2) | (r6 >> 4));
        g_pal_lookup[i][1] = (uint8_t)((g6 << 2) | (g6 >> 4));
        g_pal_lookup[i][2] = (uint8_t)((b6 << 2) | (b6 >> 4));
    }
    for (i = 0; i < 40; i++) {
        int v6 = i * 63 / 39;
        uint8_t v8 = (uint8_t)((v6 << 2) | (v6 >> 4));
        pal_set(216 + i, v6, v6, v6);
        g_pal_lookup[216 + i][0] = v8;
        g_pal_lookup[216 + i][1] = v8;
        g_pal_lookup[216 + i][2] = v8;
    }
}

static inline uint8_t rgb_to_idx(uint8_t r, uint8_t g, uint8_t b)
{
    int ri = (r * 5 + 127) / 255;
    int gi = (g * 5 + 127) / 255;
    int bi = (b * 5 + 127) / 255;
    if (ri > 5) ri = 5;
    if (gi > 5) gi = 5;
    if (bi > 5) bi = 5;
    return (uint8_t)(ri * 36 + gi * 6 + bi);
}

/* ---- VESA banked mode ----------------------------------------------- */

static int vesa_init(void)
{
    __dpmi_regs r;
    unsigned long mode_lin;
    unsigned char *mode;
    unsigned short gran, win;

    if (!g_mode_alloc) {
        g_mode_seg.size = 16;
        if (_go32_dpmi_allocate_dos_memory(&g_mode_seg) != 0)
            return -1;
        g_mode_alloc = 1;
    }

    mode_lin = (unsigned long)g_mode_seg.rm_segment * 16UL;
    mode = (unsigned char *)(__djgpp_conventional_base + mode_lin);

    r.x.ax = 0x4F02;
    r.x.bx = VESA_MODE_640;
    r.x.di = 0;
    __dpmi_int(0x10, &r);
    if (r.x.ax != 0x004F) {
        logError("VESA: failed to set mode 0x101 (AX=0x%04X)\n", r.x.ax);
        return -1;
    }

    r.x.ax = 0x4F01;
    r.x.cx = VESA_MODE_640;
    r.x.di = 0;
    r.x.es = g_mode_seg.rm_segment;
    __dpmi_int(0x10, &r);
    if (r.x.ax != 0x004F) return -1;

    gran = *(unsigned short *)(mode + 0x04);
    win  = *(unsigned short *)(mode + 0x06);

    g_bank_gran   = gran ? (int)gran * 1024 : 64 * 1024;
    g_window_size = win  ? (int)win  * 1024 : 64 * 1024;
    g_cur_bank    = -1;
    g_vga         = (uint8_t *)(__djgpp_conventional_base + VGA_WINDOW);

    return 0;
}

static inline void vesa_set_bank(int bank)
{
    __dpmi_regs r;
    if (bank == g_cur_bank) return;
    r.x.ax = 0x4F05;
    r.x.bx = 0;
    r.x.dx = (unsigned short)bank;
    __dpmi_int(0x10, &r);
    g_cur_bank = bank;
}

static void vesa_blit(void)
{
    unsigned long total = (unsigned long)SCREEN_W * SCREEN_H;
    unsigned long done  = 0;

    while (done < total) {
        unsigned long win_off  = done % (unsigned long)g_window_size;
        unsigned long win_lin  = done - win_off;
        int           bank     = (int)(win_lin / (unsigned long)g_bank_gran);
        unsigned long bank_off = done - (unsigned long)bank * (unsigned long)g_bank_gran;
        unsigned long to_win   = (unsigned long)g_window_size - win_off;
        unsigned long to_bank  = (unsigned long)g_bank_gran   - bank_off;
        unsigned long chunk    = to_win < to_bank ? to_win : to_bank;
        unsigned long remain   = total - done;

        if (chunk > remain) chunk = remain;

        vesa_set_bank(bank);
        memcpy(g_vga + bank_off, g_fb + done, chunk);
        done += chunk;
    }
}

/* ---- Texture cache -------------------------------------------------- */

static void tex_cache_init(DataWin *dw)
{
    uint32_t i;
    if (g_tex) return;
    g_tex_count = dw->txtr.count;
    if (g_tex_count == 0) return;
    g_tex = (TexPage *)safeCalloc(g_tex_count, sizeof(TexPage));
    for (i = 0; i < g_tex_count; i++) {
        g_tex[i].rgba   = NULL;
        g_tex[i].w      = 0;
        g_tex[i].h      = 0;
        g_tex[i].loaded = false;
    }
}

static int tex_evict_lru(void)
{
    uint64_t oldest = (uint64_t)-1;
    int      best   = -1;
    uint32_t i;
    for (i = 0; i < g_tex_count; i++) {
        if (g_tex[i].loaded && g_tex[i].rgba && g_tex[i].last_use < oldest) {
            oldest = g_tex[i].last_use;
            best   = (int)i;
        }
    }
    if (best < 0) return 0;
    free(g_tex[best].rgba);
    g_tex[best].rgba     = NULL;
    g_tex[best].w        = 0;
    g_tex[best].h        = 0;
    g_tex[best].loaded   = false;
    g_tex[best].last_use = 0;
    return 1;
}

static void tex_ensure(DataWin *dw, int32_t page_id)
{
    Texture *tex;
    uint8_t *rgba;
    int w = 0, h = 0;

    if (page_id < 0 || (uint32_t)page_id >= g_tex_count) return;
    if (g_tex[page_id].loaded) {
        g_tex[page_id].last_use = ++g_tex_tick;
        return;
    }

    DataWin_loadTxtrIfNeeded(dw, (uint32_t)page_id);

    tex = &dw->txtr.textures[page_id];
    if (!tex->present || !tex->blobData || tex->blobSize == 0) {
        logError("tex_ensure: page %d present=%d blob=%p size=%u\n",
                 page_id, tex->present ? 1 : 0,
                 (void*)tex->blobData, (unsigned)tex->blobSize);
        g_tex[page_id].loaded = true;
        return;
    }

    rgba = ImageDecoder_decodeToRgba(tex->blobData, tex->blobSize,
                                     false, &w, &h);

    while ((!rgba || w <= 0 || h <= 0) && tex_evict_lru()) {
        rgba = ImageDecoder_decodeToRgba(tex->blobData, tex->blobSize,
                                         false, &w, &h);
    }

    if (!rgba || w <= 0 || h <= 0) {
        /* Do NOT mark as loaded - a later attempt may succeed once
           other pages have been freed. Just log and return. */
        logError("tex_ensure: page %d decode FAILED (w=%d h=%d)\n",
                 page_id, w, h);
        return;
    }

    logInfo("tex_ensure: page %d decoded %dx%d\n", page_id, w, h);

    g_tex[page_id].rgba     = rgba;
    g_tex[page_id].w        = w;
    g_tex[page_id].h        = h;
    g_tex[page_id].loaded   = true;
    g_tex[page_id].last_use = ++g_tex_tick;
}

static void tex_cache_free(void)
{
    uint32_t i;
    if (!g_tex) return;
    for (i = 0; i < g_tex_count; i++)
        if (g_tex[i].rgba) free(g_tex[i].rgba);
        free(g_tex);
    g_tex = NULL;
    g_tex_count = 0;
}

/* ---- Sprite blit ---------------------------------------------------- */

static void blit_sprite_part(Renderer *renderer,
                             int32_t tpagIndex,
                             int32_t srcOffX, int32_t srcOffY,
                             int32_t srcW,    int32_t srcH,
                             float   x,       float   y,
                             float   xscale,  float   yscale,
                             uint32_t color,  float   alpha)
{
    DataWin        *dw  = renderer->dataWin;
    TexturePageItem *tpag;
    int              page_id;
    uint8_t         *tex;
    int              texw, texh;
    uint8_t          cr, cg, cb;
    int              dw_scaled, dh_scaled;
    int              x0, y0;
    int              dx, dy;
    int              a8;
    int              ia8;

    if (!dw || tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return;
    tpag = &dw->tpag.items[tpagIndex];
    if (!tpag->present) return;



    page_id = tpag->texturePageId;
    if (page_id < 0) return;

    tex_ensure(dw, page_id);
    if (page_id >= (int)g_tex_count || !g_tex[page_id].loaded) return;

    tex  = g_tex[page_id].rgba;
    texw = g_tex[page_id].w;
    texh = g_tex[page_id].h;
    if (!tex) return;

    if (srcW <= 0 || srcH <= 0) return;
    if (xscale <= 0.0f || yscale <= 0.0f) return;

    if (g_in_view) {
        x = (float)g_port_x + (x - (float)g_view_x) * g_view_scale_x;
        y = (float)g_port_y + (y - (float)g_view_y) * g_view_scale_y;
        xscale *= g_view_scale_x;
        yscale *= g_view_scale_y;
    }

    /* GameMaker stores colors as 0x00BBGGRR: R is the low byte, B is high. */
    cb = (uint8_t)((color >> 16) & 0xFF);
    cg = (uint8_t)((color >>  8) & 0xFF);
    cr = (uint8_t)( color        & 0xFF);

    /* Precompute draw alpha as 0..256 fixed point. */
    a8 = (int)(alpha * 256.0f + 0.5f);
    if (a8 < 0)   a8 = 0;
    if (a8 > 256) a8 = 256;

    dw_scaled = (int)(srcW * xscale + 0.5f);
    dh_scaled = (int)(srcH * yscale + 0.5f);
    x0 = (int)(x + 0.5f);
    y0 = (int)(y + 0.5f);

    if (dw_scaled <= 0 || dh_scaled <= 0) return;

    for (dy = 0; dy < dh_scaled; dy++) {
        int sy = srcOffY + (int)((float)dy / yscale);
        int fy = y0 + dy;
        const uint8_t *tex_row;
        uint8_t       *dst_row;

        if (sy < 0 || sy >= texh) continue;
        if (fy < 0 || fy >= SCREEN_H) continue;

        tex_row = tex + ((size_t)sy * (size_t)texw) * 4;
        dst_row = g_fb + (size_t)fy * SCREEN_W;

        for (dx = 0; dx < dw_scaled; dx++) {
            int sx = srcOffX + (int)((float)dx / xscale);
            int fx = x0 + dx;
            const uint8_t *p;
            uint8_t pr, pg, pb, pa;
            uint8_t tr, tg, tb;
            int pa8;

            if (sx < 0 || sx >= texw) continue;
            if (fx < 0 || fx >= SCREEN_W) continue;

            p  = tex_row + (size_t)sx * 4;
            pr = p[0];
            pg = p[1];
            pb = p[2];
            pa = p[3];

            /* Combined alpha = texture alpha * draw alpha. */
            pa8 = (pa * a8) >> 8;
            if (pa8 == 0) continue;

            tr = (uint8_t)(((unsigned)pr * (unsigned)cr) / 255u);
            tg = (uint8_t)(((unsigned)pg * (unsigned)cg) / 255u);
            tb = (uint8_t)(((unsigned)pb * (unsigned)cb) / 255u);

            if (pa8 >= 255) {
                /* Fully opaque: direct write. */
                dst_row[fx] = rgb_to_idx(tr, tg, tb);
            } else {
                /* Blend against existing pixel. */
                uint8_t old   = dst_row[fx];
                int     old_r = g_pal_lookup[old][0];
                int     old_g = g_pal_lookup[old][1];
                int     old_b = g_pal_lookup[old][2];
                ia8 = 255 - pa8;
                tr = (uint8_t)(((int)tr * pa8 + old_r * ia8) / 255);
                tg = (uint8_t)(((int)tg * pa8 + old_g * ia8) / 255);
                tb = (uint8_t)(((int)tb * pa8 + old_b * ia8) / 255);
                dst_row[fx] = rgb_to_idx(tr, tg, tb);
            }
        }
    }
}

/* ---- Tile rendering ------------------------------------------------- */

static void dosDrawTile(Renderer *r, RoomTile *tile,
                        float offsetX, float offsetY)
{
    DataWin         *dw = r->dataWin;
    int32_t          tpagIndex;
    TexturePageItem *tpag;
    static int       logged = 0;
    int              do_log = (logged < 20);

    if (!dw || !tile) return;

    tpagIndex = Renderer_resolveObjectTPAGIndex(dw, tile);

    if (do_log) {
        logInfo("drawTile: bd=%d src=(%d,%d %ux%u) dst=(%d,%d) "
                "scale=(%.2f,%.2f) tpag=%d\n",
                tile->backgroundDefinition,
                tile->sourceX, tile->sourceY,
                (unsigned)tile->width, (unsigned)tile->height,
                tile->x, tile->y,
                tile->scaleX, tile->scaleY,
                tpagIndex);
    }

    if (tpagIndex < 0) {
        if (do_log) logInfo("drawTile: tpagIndex < 0, skipping\n");
        logged++;
        return;
    }

    tpag = &dw->tpag.items[tpagIndex];
    if (!tpag->present) {
        if (do_log) logInfo("drawTile: tpag not present\n");
        logged++;
        return;
    }

    if (do_log) logInfo("drawTile: tpag present, page_id=%d src=%dx%d\n",
                        tpag->texturePageId,
                        (int)tpag->sourceWidth, (int)tpag->sourceHeight);

    /* tile->sourceX/Y are in the background's own pixel space.
       Convert to texture-page space by adding the tpag's page offset
       and subtracting its content-border offset. */
    {
        int32_t pageX = (int32_t)tpag->sourceX
                      + (tile->sourceX - (int32_t)tpag->targetX);
        int32_t pageY = (int32_t)tpag->sourceY
                      + (tile->sourceY - (int32_t)tpag->targetY);

        if (do_log) logInfo("drawTile: pageOff=(%d,%d) -> page=(%d,%d)\n",
                            (int)tpag->sourceX, (int)tpag->sourceY,
                            pageX, pageY);

        blit_sprite_part(r, tpagIndex,
                         pageX, pageY,
                         (int32_t)tile->width, (int32_t)tile->height,
                         (float)tile->x + offsetX,
                         (float)tile->y + offsetY,
                         tile->scaleX, tile->scaleY,
                         tile->color & 0x00FFFFFFu,
                         tile->alpha);
    }

    if (do_log) logged++;
}

/* ---- Surface bookkeeping -------------------------------------------- */

static void dosDrawTiledPart(Renderer *r, int32_t a, int32_t b, int32_t c,
                             int32_t d, int32_t e, float f, float g,
                             float h, float i, uint32_t j, float k)
{ (void)r;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;
  (void)h;(void)i;(void)j;(void)k; }


static void dosEnsureSurfaceCapacity(DOSRenderer *d, uint32_t needed)
{
    if (needed <= d->surfaceCapacity) return;
    {
        uint32_t newCap = d->surfaceCapacity ? d->surfaceCapacity * 2 : 16;
        uint32_t i;
        while (newCap < needed) newCap *= 2;
        d->surfaceWidths     = (int32_t *)safeRealloc(d->surfaceWidths,
                                                      newCap * sizeof(int32_t));
        d->surfaceHeights    = (int32_t *)safeRealloc(d->surfaceHeights,
                                                      newCap * sizeof(int32_t));
        d->surfaceExistsFlag = (bool *)safeRealloc(d->surfaceExistsFlag,
                                                   newCap * sizeof(bool));
        for (i = d->surfaceCapacity; i < newCap; i++) {
            d->surfaceWidths[i]     = 0;
            d->surfaceHeights[i]    = 0;
            d->surfaceExistsFlag[i] = false;
        }
        d->surfaceCapacity = newCap;
        if (needed > d->surfaceCount) d->surfaceCount = needed;
    }
}

/* ---- Init / destroy ------------------------------------------------- */

static void dosInit(Renderer *renderer, DataWin *dataWin)
{
    Matrix4f world;

    renderer->dataWin = dataWin;

    if (__djgpp_nearptr_enable() == 0) {
        logError("DOS renderer: nearptr enable failed\n");
        return;
    }
    g_nearptr = 1;

    if (vesa_init() != 0) {
        logError("DOS renderer: VESA init failed\n");
        return;
    }

    pal_install();

    g_fb = (uint8_t *)malloc((size_t)SCREEN_W * SCREEN_H);
    if (!g_fb) {
        logError("DOS renderer: framebuffer alloc failed\n");
        return;
    }
    memset(g_fb, 0, (size_t)SCREEN_W * SCREEN_H);

    tex_cache_init(dataWin);

    Matrix4f_identity(&world);
    renderer->gmlMatrices[MATRIX_WORLD] = world;

    logInfo("DOS renderer initialized: VESA 640x480x8, %u texture pages\n",
            (unsigned)g_tex_count);
}

static void dosDestroy(Renderer *renderer)
{
    DOSRenderer *d = (DOSRenderer *)renderer;

    if (g_fb) { free(g_fb); g_fb = NULL; }
    tex_cache_free();

    {
        __dpmi_regs r;
        r.x.ax = 0x0003;
        __dpmi_int(0x10, &r);
    }

    if (g_nearptr) {
        __djgpp_nearptr_disable();
        g_nearptr = 0;
    }

    if (g_mode_alloc) {
        _go32_dpmi_free_dos_memory(&g_mode_seg);
        g_mode_alloc = 0;
    }

    free(d->surfaceWidths);
    free(d->surfaceHeights);
    free(d->surfaceExistsFlag);
    free(d);
}

/* ---- Frame hooks ---------------------------------------------------- */

static void dosBeginFrame(Renderer *r, int32_t gw, int32_t gh, int32_t ww, int32_t wh)
{
    (void)r; (void)gw; (void)gh; (void)ww; (void)wh;

    /* Cap loaded texture pages at 10. As the player moves through rooms,
       pages used only by previous rooms age out and get freed. */
    {
        int loaded = 0;
        uint32_t i;
        for (i = 0; i < g_tex_count; i++)
            if (g_tex[i].loaded && g_tex[i].rgba) loaded++;
        while (loaded > 10 && tex_evict_lru())
            loaded--;
    }
}
static void dosEndFrameInit(Renderer *r) { (void)r; }

/* ---- FPS counter ---------------------------------------------------- */

static void dos_draw_digit(int x, int y, int d, uint8_t color)
{
    static const unsigned char glyph[10][7] = {
        { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },
        { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
        { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },
        { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
        { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },
        { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
        { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },
        { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
        { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
        { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
    };
    int row, col;
    if (d < 0 || d > 9) return;
    for (row = 0; row < 7; row++) {
        unsigned char bits = glyph[d][row];
        for (col = 0; col < 5; col++) {
            if (bits & (1 << (4 - col))) {
                int fx = x + col, fy = y + row;
                if (fx >= 0 && fx < SCREEN_W && fy >= 0 && fy < SCREEN_H)
                    g_fb[(size_t)fy * SCREEN_W + fx] = color;
            }
        }
    }
}

static void dos_draw_fps(void)
{
    int x = 4, y = 4;
    int i, j;
    uint32_t v = g_fps_value;

    for (j = y - 2; j < y + 9; j++)
        for (i = x - 2; i < x + 22; i++)
            if (i >= 0 && i < SCREEN_W && j >= 0 && j < SCREEN_H)
                g_fb[(size_t)j * SCREEN_W + i] = 0;

    if (v > 999) v = 999;
    dos_draw_digit(x,      y, (v / 100) % 10, 255);
    dos_draw_digit(x + 6,  y, (v / 10) % 10,  255);
    dos_draw_digit(x + 12, y,  v % 10,        255);
}

static void dosEndFrameEnd(Renderer *r)
{
    (void)r;

    {
        uint64_t now = nowNanos();
        g_fps_frames++;
        if (g_fps_last_ns == 0) {
            g_fps_last_ns = now;
        } else if (now - g_fps_last_ns >= 1000000000ULL) {
            g_fps_value  = (uint32_t)((uint64_t)g_fps_frames * 1000000000ULL
            / (now - g_fps_last_ns));
            g_fps_frames = 0;
            g_fps_last_ns = now;
        }
    }

    if (g_dos_fps_enabled && g_fb)
        dos_draw_fps();

    if (g_fb && g_vga)
        vesa_blit();
}

/* ---- View hooks ----------------------------------------------------- */

static void dosBeginView(Renderer *r, int32_t viewX, int32_t viewY,
                         int32_t viewW, int32_t viewH,
                         int32_t portX, int32_t portY,
                         int32_t portW, int32_t portH, float viewAngle)
{
    (void)r; (void)viewAngle;
    g_view_x = viewX; g_view_y = viewY;
    g_view_w = viewW; g_view_h = viewH;
    g_port_x = portX; g_port_y = portY;
    g_port_w = portW; g_port_h = portH;
    g_view_scale_x = (viewW > 0) ? (float)portW / (float)viewW : 1.0f;
    g_view_scale_y = (viewH > 0) ? (float)portH / (float)viewH : 1.0f;
    g_in_view = 1;
}

static void dosEndView(Renderer *r) { (void)r; g_in_view = 0; }
static void dosApplyProjection(Renderer *r, const Matrix4f *v, const Matrix4f *p)
{ (void)r; (void)v; (void)p; }
static void dosBeginGUI(Renderer *r, int32_t a, int32_t b, int32_t c, int32_t d,
                        int32_t e, int32_t f, int32_t g)
{ (void)r;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g; }
static void dosSetGuiProjection(Renderer *r, int32_t a, int32_t b, int32_t c, int32_t d, bool e)
{ (void)r;(void)a;(void)b;(void)c;(void)d;(void)e; }
static void dosEndGUI(Renderer *r) { (void)r; }

/* ---- Sprite draws --------------------------------------------------- */

static void dosDrawSprite(Renderer *r, int32_t tpagIndex,
                          float x, float y,
                          float originX, float originY,
                          float xscale, float yscale,
                          float angleDeg, uint32_t color, float alpha)
{
    DataWin *dw = r->dataWin;
    TexturePageItem *tpag;
    float dx, dy;
    (void)angleDeg;

    if (!dw || tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return;
    tpag = &dw->tpag.items[tpagIndex];
    if (!tpag->present) return;



    dx = x + ((float)tpag->targetX - originX) * xscale;
    dy = y + ((float)tpag->targetY - originY) * yscale;

    blit_sprite_part(r, tpagIndex,
                     (int32_t)tpag->sourceX, (int32_t)tpag->sourceY,
                     (int32_t)tpag->sourceWidth, (int32_t)tpag->sourceHeight,
                     dx, dy, xscale, yscale, color, alpha);
}

static void dosDrawSpritePart(Renderer *r, int32_t tpagIndex,
                              int32_t srcOffX, int32_t srcOffY,
                              int32_t srcW, int32_t srcH,
                              float x, float y,
                              float xscale, float yscale,
                              float angleDeg,
                              float pivotX, float pivotY,
                              uint32_t color, float alpha)
{
    (void)angleDeg; (void)pivotX; (void)pivotY;
    blit_sprite_part(r, tpagIndex, srcOffX, srcOffY, srcW, srcH,
                     x, y, xscale, yscale, color, alpha);
}

static void dosDrawSpritePartColor(Renderer *r, int32_t tpagIndex,
                                   int32_t srcOffX, int32_t srcOffY,
                                   int32_t srcW, int32_t srcH,
                                   float x, float y,
                                   float xscale, float yscale,
                                   float angleDeg,
                                   float pivotX, float pivotY,
                                   uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4,
                                   float alpha)
{
    (void)c2; (void)c3; (void)c4;
    dosDrawSpritePart(r, tpagIndex, srcOffX, srcOffY, srcW, srcH,
                      x, y, xscale, yscale, angleDeg, pivotX, pivotY,
                      c1, alpha);
}

static void dosDrawSpritePos(Renderer *r, int32_t tpagIndex,
                             float x1, float y1, float x2, float y2,
                             float x3, float y3, float x4, float y4,
                             float alpha)
{
    float minx = x1, maxx = x1, miny = y1, maxy = y1;
    DataWin *dw = r->dataWin;
    TexturePageItem *tpag;

    if (x2 < minx) minx = x2; if (x2 > maxx) maxx = x2;
    if (x3 < minx) minx = x3; if (x3 > maxx) maxx = x3;
    if (x4 < minx) minx = x4; if (x4 > maxx) maxx = x4;
    if (y2 < miny) miny = y2; if (y2 > maxy) maxy = y2;
    if (y3 < miny) miny = y3; if (y3 > maxy) maxy = y3;
    if (y4 < miny) miny = y4; if (y4 > maxy) maxy = y4;

    if (!dw || tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return;
    tpag = &dw->tpag.items[tpagIndex];
    if (!tpag->present || tpag->sourceWidth == 0 || tpag->sourceHeight == 0) return;

    blit_sprite_part(r, tpagIndex,
                     (int32_t)tpag->sourceX, (int32_t)tpag->sourceY,
                     (int32_t)tpag->sourceWidth, (int32_t)tpag->sourceHeight,
                     minx, miny,
                     (maxx - minx) / (float)tpag->sourceWidth,
                     (maxy - miny) / (float)tpag->sourceHeight,
                     0xFFFFFF, alpha);
}

/* ---- Primitives ----------------------------------------------------- */

static void dosDrawRectangle(Renderer *r, float x1, float y1, float x2, float y2,
                             uint32_t color, float alpha, bool outline)
{
    int ix1, iy1, ix2, iy2, x, y;
    uint8_t cr, cg, cb, idx;
    int a8;
    (void)r;
    if (!g_fb) return;

    if (g_in_view) {
        x1 = (float)g_port_x + (x1 - (float)g_view_x) * g_view_scale_x;
        y1 = (float)g_port_y + (y1 - (float)g_view_y) * g_view_scale_y;
        x2 = (float)g_port_x + (x2 - (float)g_view_x) * g_view_scale_x;
        y2 = (float)g_port_y + (y2 - (float)g_view_y) * g_view_scale_y;
    }

    if (x1 > x2) { float t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { float t = y1; y1 = y2; y2 = t; }

    ix1 = (int)x1; iy1 = (int)y1;
    ix2 = (int)x2; iy2 = (int)y2;

    if (ix1 < 0) ix1 = 0;
    if (iy1 < 0) iy1 = 0;
    if (ix2 > SCREEN_W) ix2 = SCREEN_W;
    if (iy2 > SCREEN_H) iy2 = SCREEN_H;
    if (ix1 >= ix2 || iy1 >= iy2) return;

    /* BGR source. */
    cb = (uint8_t)((color >> 16) & 0xFF);
    cg = (uint8_t)((color >>  8) & 0xFF);
    cr = (uint8_t)( color        & 0xFF);

    a8 = (int)(alpha * 256.0f + 0.5f);
    if (a8 < 0) a8 = 0;
    if (a8 > 256) a8 = 256;

    if (outline || a8 >= 255) {
        idx = rgb_to_idx(cr, cg, cb);
        if (outline) {
            for (x = ix1; x < ix2; x++) {
                g_fb[(size_t)iy1 * SCREEN_W + x]       = idx;
                g_fb[(size_t)(iy2 - 1) * SCREEN_W + x] = idx;
            }
            for (y = iy1; y < iy2; y++) {
                g_fb[(size_t)y * SCREEN_W + ix1]       = idx;
                g_fb[(size_t)y * SCREEN_W + (ix2 - 1)] = idx;
            }
        } else {
            for (y = iy1; y < iy2; y++)
                memset(g_fb + (size_t)y * SCREEN_W + ix1, idx,
                       (size_t)(ix2 - ix1));
        }
    } else {
        /* Alpha-blended fill. */
        int ia8 = 255 - (a8 >> 0);
        if (ia8 < 0) ia8 = 0;
        if (ia8 > 255) ia8 = 255;
        for (y = iy1; y < iy2; y++) {
            uint8_t *row = g_fb + (size_t)y * SCREEN_W;
            for (x = ix1; x < ix2; x++) {
                uint8_t old   = row[x];
                int     old_r = g_pal_lookup[old][0];
                int     old_g = g_pal_lookup[old][1];
                int     old_b = g_pal_lookup[old][2];
                uint8_t nr = (uint8_t)(((int)cr * (a8 >> 0) + old_r * ia8) / 255);
                uint8_t ng = (uint8_t)(((int)cg * (a8 >> 0) + old_g * ia8) / 255);
                uint8_t nb = (uint8_t)(((int)cb * (a8 >> 0) + old_b * ia8) / 255);
                row[x] = rgb_to_idx(nr, ng, nb);
            }
        }
    }
}

static void dosDrawRectangleColor(Renderer *r, float x1, float y1, float x2, float y2,
                                  uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4,
                                  float alpha, bool outline)
{
    (void)c2; (void)c3; (void)c4;
    dosDrawRectangle(r, x1, y1, x2, y2, c1, alpha, outline);
}

static void dosDrawLine(Renderer *r, float x1, float y1, float x2, float y2,
                        float width, uint32_t color, float alpha)
{
    int ix1, iy1, ix2, iy2;
    int dx, dy, sx, sy, err;
    uint8_t cr, cg, cb, idx;
    (void)r; (void)alpha; (void)width;
    if (!g_fb) return;

    if (g_in_view) {
        x1 = (float)g_port_x + (x1 - (float)g_view_x) * g_view_scale_x;
        y1 = (float)g_port_y + (y1 - (float)g_view_y) * g_view_scale_y;
        x2 = (float)g_port_x + (x2 - (float)g_view_x) * g_view_scale_x;
        y2 = (float)g_port_y + (y2 - (float)g_view_y) * g_view_scale_y;
    }

    ix1 = (int)x1; iy1 = (int)y1;
    ix2 = (int)x2; iy2 = (int)y2;

    dx = abs(ix2 - ix1);
    dy = -abs(iy2 - iy1);
    sx = ix1 < ix2 ? 1 : -1;
    sy = iy1 < iy2 ? 1 : -1;
    err = dx + dy;

    cb = (uint8_t)((color >> 16) & 0xFF);
    cg = (uint8_t)((color >>  8) & 0xFF);
    cr = (uint8_t)( color        & 0xFF);
    idx = rgb_to_idx(cr, cg, cb);

    for (;;) {
        if (ix1 >= 0 && ix1 < SCREEN_W && iy1 >= 0 && iy1 < SCREEN_H)
            g_fb[(size_t)iy1 * SCREEN_W + ix1] = idx;
        if (ix1 == ix2 && iy1 == iy2) break;
        {
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; ix1 += sx; }
            if (e2 <= dx) { err += dx; iy1 += sy; }
        }
    }
}

static void dosDrawLineColor(Renderer *r, float x1, float y1, float x2, float y2,
                             float width, uint32_t c1, uint32_t c2, float alpha)
{
    (void)c2;
    dosDrawLine(r, x1, y1, x2, y2, width, c1, alpha);
}

static void dosDrawTriangle(Renderer *r, float x1, float y1, float x2, float y2,
                            float x3, float y3,
                            uint32_t c1, uint32_t c2, uint32_t c3,
                            float alpha, bool outline)
{
    (void)c2; (void)c3;
    if (outline) {
        dosDrawLine(r, x1, y1, x2, y2, 1.0f, c1, alpha);
        dosDrawLine(r, x2, y2, x3, y3, 1.0f, c1, alpha);
        dosDrawLine(r, x3, y3, x1, y1, 1.0f, c1, alpha);
    } else {
        dosDrawRectangle(r, x1, y1, x3, y3, c1, alpha, false);
    }
}

/* ---- Text ----------------------------------------------------------- */

static void dos_text_measure(Font *f, const char *text,
                             float xscale, float yscale,
                             float lineSeparation,
                             float *outW, float *outH, float *outLineH)
{
    float maxW = 0, lineW = 0, lineH;
    const char *p = text;

    if (f && f->hasLineHeight && f->lineHeight > 0) {
        lineH = (float)f->lineHeight * f->scaleY * yscale;
    } else if (!f || f->maxGlyphHeight == 0) {
        lineH = (lineSeparation > 0) ? lineSeparation : 16.0f;
    } else {
        lineH = (float)f->maxGlyphHeight * f->scaleY * yscale * 1.25f;
    }
    if (lineH <= 0) lineH = 16.0f;

    while (*p) {
        unsigned char c = (unsigned char)*p++;
        if (c == '\n') {
            if (lineW > maxW) maxW = lineW;
            lineW = 0;
            continue;
        }
        if (c < 128 && f) {
            FontGlyph *g = f->glyphLUT[c];
            if (g) lineW += (float)g->shift * f->scaleX * xscale;
        }
    }
    if (lineW > maxW) maxW = lineW;

    {
        int lines = 1;
        for (p = text; *p; p++) if (*p == '\n') lines++;
        if (outH) *outH = lineH * (float)lines;
    }
    if (outW) *outW = maxW;
    if (outLineH) *outLineH = lineH;
}

static void dos_draw_text_impl(Renderer *r, const char *text,
                               float x, float y,
                               float xscale, float yscale,
                               float lineSeparation,
                               uint32_t color, float alpha)
{
    DataWin *dw;
    Font    *f;
    int32_t  fontIdx;
    float    curX, curY, lineH;
    float    totalW = 0, totalH = 0;
    const char *p;
    int32_t  atlas_off_x, atlas_off_y;

    if (!g_fb || !text) return;

    dw = r->dataWin;
    if (!dw) return;

    fontIdx = r->drawFont;
    if (fontIdx < 0 || (uint32_t)fontIdx >= dw->font.count) return;
    f = &dw->font.fonts[fontIdx];
    if (!f->present || !f->glyphs || f->tpagIndex < 0) return;

    dos_text_measure(f, text, xscale, yscale, lineSeparation,
                     &totalW, &totalH, &lineH);

    if (r->drawHalign == 1) x -= totalW * 0.5f;
    else if (r->drawHalign == 2) x -= totalW;
    if (r->drawValign == 1) y -= totalH * 0.5f;
    else if (r->drawValign == 2) y -= totalH;

    atlas_off_x = (int32_t)dw->tpag.items[f->tpagIndex].sourceX;
    atlas_off_y = (int32_t)dw->tpag.items[f->tpagIndex].sourceY;

    curX = x;
    curY = y;

    for (p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        FontGlyph *g;

        if (c == '\n') {
            curX = x;
            curY += lineH;
            continue;
        }

        if (c >= 128) continue;
        g = f->glyphLUT[c];
        if (!g) continue;

        blit_sprite_part(r, f->tpagIndex,
                         atlas_off_x + (int32_t)g->sourceX,
                         atlas_off_y + (int32_t)g->sourceY,
                         (int32_t)g->sourceWidth,
                         (int32_t)g->sourceHeight,
                         curX, curY,
                         xscale * f->scaleX, yscale * f->scaleY,
                         color, alpha);

        curX += (float)g->shift * f->scaleX * xscale;
    }
}

static void dosDrawText(Renderer *r, const char *text, float x, float y,
                        float xscale, float yscale, float angleDeg,
                        float lineSeparation)
{
    (void)angleDeg;
    dos_draw_text_impl(r, text, x, y, xscale, yscale, lineSeparation,
                       r->drawColor, r->drawAlpha);
}

static void dosDrawTextColor(Renderer *r, const char *text, float x, float y,
                             float xscale, float yscale, float angleDeg,
                             int32_t c1, int32_t c2, int32_t c3, int32_t c4,
                             float alpha, float lineSeparation)
{
    (void)angleDeg; (void)c2; (void)c3; (void)c4;
    dos_draw_text_impl(r, text, x, y, xscale, yscale, lineSeparation,
                       (uint32_t)c1, alpha);
}

static void dosDrawTextUI(Renderer *r, const char *text, float x, float y,
                          float xscale, float yscale, float angleDeg,
                          int32_t c1, int32_t c2, int32_t c3, int32_t c4,
                          float alpha, float lineSeparation)
{
    (void)angleDeg; (void)c2; (void)c3; (void)c4;
    dos_draw_text_impl(r, text, x, y, xscale, yscale, lineSeparation,
                       (uint32_t)c1, alpha);
}

static void dosFlush(Renderer *r) { (void)r; }

static void dosClearScreen(Renderer *r, uint32_t color, float alpha)
{
    uint8_t cr, cg, cb, idx;
    (void)r; (void)alpha;
    if (!g_fb) return;
    cb = (uint8_t)((color >> 16) & 0xFF);
    cg = (uint8_t)((color >>  8) & 0xFF);
    cr = (uint8_t)( color        & 0xFF);
    idx = rgb_to_idx(cr, cg, cb);
    memset(g_fb, idx, (size_t)SCREEN_W * SCREEN_H);
}

/* ---- Stubs ---------------------------------------------------------- */

static int32_t dosCreateSpriteFromSurface(Renderer *r, int32_t a, int32_t b,
                                          int32_t c, int32_t d,
                                          bool e, bool f,
                                          int32_t g, int32_t h)
{ (void)r;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h; return -1; }
static void dosDeleteSprite(Renderer *r, int32_t s) { (void)r; (void)s; }

static BlendFactors dosGpuGetBlendFactors(Renderer *r)
{ return ((DOSRenderer *)r)->blendFactors; }
static int32_t dosGpuGetBlendMode(Renderer *r)
{ return ((DOSRenderer *)r)->blendMode; }
static void dosGpuSetBlendMode(Renderer *r, int32_t m)
{ ((DOSRenderer *)r)->blendMode = m; }
static void dosGpuSetBlendModeExt(Renderer *r, int32_t sf, int32_t df, int32_t sfa, int32_t dfa)
{
    DOSRenderer *d = (DOSRenderer *)r;
    d->blendFactors.src = sf; d->blendFactors.dst = df;
    d->blendFactors.srcAlpha = sfa; d->blendFactors.dstAlpha = dfa;
}
static void dosGpuSetBlendEnable(Renderer *r, bool e) { ((DOSRenderer *)r)->blendEnable = e; }
static bool dosGpuGetBlendEnable(Renderer *r) { return ((DOSRenderer *)r)->blendEnable; }
static void dosGpuSetAlphaTestEnable(Renderer *r, bool e) { ((DOSRenderer *)r)->alphaTestEnable = e; }
static bool dosGpuGetAlphaTestEnable(Renderer *r) { return ((DOSRenderer *)r)->alphaTestEnable; }
static void dosGpuSetAlphaTestRef(Renderer *r, uint8_t x) { ((DOSRenderer *)r)->alphaTestRef = x; }
static void dosGpuSetColorWriteEnable(Renderer *r, bool rr, bool gg, bool bb, bool aa)
{
    DOSRenderer *d = (DOSRenderer *)r;
    d->colorWriteR = rr; d->colorWriteG = gg;
    d->colorWriteB = bb; d->colorWriteA = aa;
}
static void dosGpuGetColorWriteEnable(Renderer *r, bool *rr, bool *gg, bool *bb, bool *aa)
{
    DOSRenderer *d = (DOSRenderer *)r;
    if (rr) *rr = d->colorWriteR;
    if (gg) *gg = d->colorWriteG;
    if (bb) *bb = d->colorWriteB;
    if (aa) *aa = d->colorWriteA;
}
static void dosGpuSetFog(Renderer *r, bool e, uint32_t c)
{ DOSRenderer *d = (DOSRenderer *)r; d->fogEnable = e; d->fogColor = c; }

static void dosDrawSpriteTiled(Renderer *r, int32_t t, float a, float b,
                               float c, float d, float e, float f,
                               bool g, bool h, float i, float j,
                               uint32_t k, float l)
{ (void)r;(void)t;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;
    (void)g;(void)h;(void)i;(void)j;(void)k;(void)l; }

    static int32_t dosCreateSurface(Renderer *r, int32_t w, int32_t h)
    {
        DOSRenderer *d = (DOSRenderer *)r;
        uint32_t i;
        for (i = 0; i < d->surfaceCount; i++) {
            if (!d->surfaceExistsFlag[i]) {
                d->surfaceWidths[i] = w; d->surfaceHeights[i] = h;
                d->surfaceExistsFlag[i] = true;
                return (int32_t)i;
            }
        }
        {
            uint32_t id = d->surfaceCount;
            dosEnsureSurfaceCapacity(d, id + 1);
            d->surfaceWidths[id] = w;
            d->surfaceHeights[id] = h;
            d->surfaceExistsFlag[id] = true;
            return (int32_t)id;
        }
    }
    static bool dosSurfaceExists(Renderer *r, int32_t id)
    {
        DOSRenderer *d = (DOSRenderer *)r;
        if (id < 0 || (uint32_t)id >= d->surfaceCount) return false;
        return d->surfaceExistsFlag[id];
    }
    static bool dosSetRenderTarget(Renderer *r, int32_t id, bool implicit)
    {
        (void)implicit;
        if (id == APPLICATION_SURFACE_ID || id == RENDER_TARGET_HOST_FRAMEBUFFER) return true;
        return dosSurfaceExists(r, id);
    }
    static int32_t dosEnsureApplicationSurface(Renderer *r, int32_t w, int32_t h)
    { (void)r; (void)w; (void)h; return APPLICATION_SURFACE_ID; }
    static float dosGetSurfaceWidth(Renderer *r, int32_t id)
    {
        DOSRenderer *d = (DOSRenderer *)r;
        if (id < 0 || (uint32_t)id >= d->surfaceCount) return 0.0f;
        if (!d->surfaceExistsFlag[id]) return 0.0f;
        return (float)d->surfaceWidths[id];
    }
    static float dosGetSurfaceHeight(Renderer *r, int32_t id)
    {
        DOSRenderer *d = (DOSRenderer *)r;
        if (id < 0 || (uint32_t)id >= d->surfaceCount) return 0.0f;
        if (!d->surfaceExistsFlag[id]) return 0.0f;
        return (float)d->surfaceHeights[id];
    }
    static void dosDrawSurface(Renderer *r, int32_t a, int32_t b, int32_t c,
                               int32_t d, int32_t e, float f, float g,
                               float h, float i, float j, uint32_t k, float l)
    { (void)r;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;
        (void)h;(void)i;(void)j;(void)k;(void)l; }
        static void dosDrawSurfaceColor(Renderer *r, int32_t a, int32_t b, int32_t c,
                                        int32_t d, int32_t e, float f, float g,
                                        float h, float i, float j,
                                        uint32_t k, uint32_t l, uint32_t m, uint32_t n,
                                        float o)
        { (void)r;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;
            (void)h;(void)i;(void)j;(void)k;(void)l;(void)m;(void)n;(void)o; }
            static void dosDrawSurfaceTiled(Renderer *r, int32_t a, float b, float c,
                                            float d, float e, float f, float g,
                                            uint32_t h, float i)
            { (void)r;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i; }
            static void dosSurfaceResize(Renderer *r, int32_t id, int32_t w, int32_t h)
            {
                DOSRenderer *d = (DOSRenderer *)r;
                if (id < 0 || (uint32_t)id >= d->surfaceCount) return;
                if (!d->surfaceExistsFlag[id]) return;
                d->surfaceWidths[id] = w;
                d->surfaceHeights[id] = h;
            }
            static void dosSurfaceFree(Renderer *r, int32_t id)
            {
                DOSRenderer *d = (DOSRenderer *)r;
                if (id < 0 || (uint32_t)id >= d->surfaceCount) return;
                d->surfaceExistsFlag[id] = false;
                d->surfaceWidths[id] = 0;
                d->surfaceHeights[id] = 0;
            }
            static void dosSurfaceCopy(Renderer *r, int32_t a, int32_t b, int32_t c,
                                       int32_t d, int32_t e, int32_t f, int32_t g,
                                       int32_t h, bool i)
            { (void)r;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i; }
            static bool dosSurfaceGetPixels(Renderer *r, int32_t id, uint8_t *out)
            { (void)r; (void)id; (void)out; return false; }
            
                static void dosGpuSetShader(Renderer *r, int32_t s) { r->currentShader = s; }
                static void dosGpuResetShader(Renderer *r) { r->currentShader = -1; }
                static int32_t dosShaderGetUniform(Renderer *r, int32_t s, char *u)
                { (void)r;(void)s;(void)u; return -1; }
                static int32_t dosShaderGetSamplerIndex(Renderer *r, int32_t s, char *u)
                { (void)r;(void)s;(void)u; return -1; }
                static void dosShaderSetUniformF(Renderer *r, int32_t h, int32_t c,
                                                 float a, float b, float cc, float d)
                { (void)r;(void)h;(void)c;(void)a;(void)b;(void)cc;(void)d; }
                static void dosShaderSetUniformFArray(Renderer *r, int32_t h, float *v, uint32_t n)
                { (void)r;(void)h;(void)v;(void)n; }
                static void dosShaderSetUniformI(Renderer *r, int32_t h, int32_t c,
                                                 int32_t a, int32_t b, int32_t cc, int32_t d)
                { (void)r;(void)h;(void)c;(void)a;(void)b;(void)cc;(void)d; }
                static uint32_t dosSpriteGetTexture(Renderer *r, int32_t t)
                { (void)r; (void)t; return 0; }
                static uint32_t dosSurfaceGetTexture(Renderer *r, int32_t s) { (void)r; (void)s; return 0; }
                static float dosTextureGetTexelWidth(Renderer *r, uint32_t t) { (void)r; (void)t; return 1.0f; }
                static float dosTextureGetTexelHeight(Renderer *r, uint32_t t) { (void)r; (void)t; return 1.0f; }
                static bool dosTextureGetUVs(Renderer *r, uint32_t t, float *u)
                { (void)r; (void)t; (void)u; return false; }
                static void dosTextureSetStage(Renderer *r, int32_t s, uint32_t t)
                { (void)r; (void)s; (void)t; }
                static bool dosShaderIsCompiled(Renderer *r, int32_t s) { (void)r; (void)s; return false; }
                static bool dosShadersSupported(void) { return false; }
                static void dosSetMatrix(Renderer *r, int32_t t, Matrix4f m)
                { if (t >= 0 && t < MATRICES_MAX) r->gmlMatrices[t] = m; }

                /* ---- Creation ------------------------------------------------------- */

                static RendererVtable dosVtable;

                Renderer* DOSRenderer_create(void)
                {
                    DOSRenderer *d = (DOSRenderer *)safeCalloc(1, sizeof(DOSRenderer));
                    d->base.vtable = &dosVtable;

                    dosVtable.init = dosInit;
                    dosVtable.destroy = dosDestroy;
                    dosVtable.beginFrame = dosBeginFrame;
                    dosVtable.endFrameInit = dosEndFrameInit;
                    dosVtable.endFrameEnd = dosEndFrameEnd;
                    dosVtable.beginView = dosBeginView;
                    dosVtable.endView = dosEndView;
                    dosVtable.applyProjection = dosApplyProjection;
                    dosVtable.beginGUI = dosBeginGUI;
                    dosVtable.setGuiProjection = dosSetGuiProjection;
                    dosVtable.endGUI = dosEndGUI;
                    dosVtable.drawSprite = dosDrawSprite;
                    dosVtable.drawSpritePart = dosDrawSpritePart;
                    dosVtable.drawSpritePartColor = dosDrawSpritePartColor;
                    dosVtable.drawSpritePos = dosDrawSpritePos;
                    dosVtable.drawRectangle = dosDrawRectangle;
                    dosVtable.drawRectangleColor = dosDrawRectangleColor;
                    dosVtable.drawLine = dosDrawLine;
                    dosVtable.drawTriangle = dosDrawTriangle;
                    dosVtable.drawLineColor = dosDrawLineColor;
                    dosVtable.drawText = dosDrawText;
                    dosVtable.drawTextColor = dosDrawTextColor;
                    dosVtable.drawTextUI = dosDrawTextUI;
                    dosVtable.flush = dosFlush;
                    dosVtable.clearScreen = dosClearScreen;
                    dosVtable.createSpriteFromSurface = dosCreateSpriteFromSurface;
                    dosVtable.deleteSprite = dosDeleteSprite;
                    dosVtable.gpuGetBlendFactors = dosGpuGetBlendFactors;
                    dosVtable.gpuGetBlendMode = dosGpuGetBlendMode;
                    dosVtable.gpuSetBlendMode = dosGpuSetBlendMode;
                    dosVtable.gpuSetBlendModeExt = dosGpuSetBlendModeExt;
                    dosVtable.gpuSetBlendEnable = dosGpuSetBlendEnable;
                    dosVtable.gpuGetBlendEnable = dosGpuGetBlendEnable;
                    dosVtable.gpuSetAlphaTestEnable = dosGpuSetAlphaTestEnable;
                    dosVtable.gpuGetAlphaTestEnable = dosGpuGetAlphaTestEnable;
                    dosVtable.gpuSetAlphaTestRef = dosGpuSetAlphaTestRef;
                    dosVtable.gpuSetColorWriteEnable = dosGpuSetColorWriteEnable;
                    dosVtable.gpuGetColorWriteEnable = dosGpuGetColorWriteEnable;
                    dosVtable.gpuSetFog = dosGpuSetFog;
                    dosVtable.drawSpriteTiled = dosDrawSpriteTiled;
    dosVtable.drawTile = dosDrawTile;
                    dosVtable.createSurface = dosCreateSurface;
                    dosVtable.surfaceExists = dosSurfaceExists;
                    dosVtable.setRenderTarget = dosSetRenderTarget;
                    dosVtable.ensureApplicationSurface = dosEnsureApplicationSurface;
                    dosVtable.getSurfaceWidth = dosGetSurfaceWidth;
                    dosVtable.getSurfaceHeight = dosGetSurfaceHeight;
                    dosVtable.drawSurface = dosDrawSurface;
                    dosVtable.drawSurfaceColor = dosDrawSurfaceColor;
                    dosVtable.drawSurfaceTiled = dosDrawSurfaceTiled;
                    dosVtable.surfaceResize = dosSurfaceResize;
                    dosVtable.surfaceFree = dosSurfaceFree;
                    dosVtable.surfaceCopy = dosSurfaceCopy;
                    dosVtable.surfaceGetPixels = dosSurfaceGetPixels;
                    dosVtable.drawTiledPart = dosDrawTiledPart;
                    dosVtable.gpuSetShader = dosGpuSetShader;
                    dosVtable.gpuResetShader = dosGpuResetShader;
                    dosVtable.shaderGetUniform = dosShaderGetUniform;
                    dosVtable.shaderGetSamplerIndex = dosShaderGetSamplerIndex;
                    dosVtable.shaderSetUniformF = dosShaderSetUniformF;
                    dosVtable.shaderSetUniformFArray = dosShaderSetUniformFArray;
                    dosVtable.shaderSetUniformI = dosShaderSetUniformI;
                    dosVtable.spriteGetTexture = dosSpriteGetTexture;
                    dosVtable.surfaceGetTexture = dosSurfaceGetTexture;
                    dosVtable.textureGetTexelWidth = dosTextureGetTexelWidth;
                    dosVtable.textureGetTexelHeight = dosTextureGetTexelHeight;
                    dosVtable.textureGetUVs = dosTextureGetUVs;
                    dosVtable.textureSetStage = dosTextureSetStage;
                    dosVtable.shaderIsCompiled = dosShaderIsCompiled;
                    dosVtable.shadersSupported = dosShadersSupported;
                    dosVtable.setMatrix = dosSetMatrix;

                    d->base.drawColor = 0xFFFFFF;
                    d->base.drawAlpha = 1.0f;
                    d->base.drawFont = -1;
                    d->base.drawHalign = 0;
                    d->base.drawValign = 0;
                    d->base.circlePrecision = 24;
                    d->base.currentShader = -1;
                    Matrix4f_identity(&d->base.gmlMatrices[MATRIX_WORLD]);

                    d->blendEnable = true;
                    d->blendMode = bm_normal;
                    d->blendFactors.src = bm_src_alpha;
                    d->blendFactors.dst = bm_inv_src_alpha;
                    d->blendFactors.srcAlpha = bm_src_alpha;
                    d->blendFactors.dstAlpha = bm_inv_src_alpha;
                    d->alphaTestEnable = false;
                    d->alphaTestRef = 0;
                    d->colorWriteR = d->colorWriteG = d->colorWriteB = d->colorWriteA = true;
                    d->fogEnable = false;
                    d->fogColor = 0;

                    return (Renderer *)d;
                }
