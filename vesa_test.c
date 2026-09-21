/*
 * vesa_test.c – Read-only VESA/VBE probe. Does NOT set a mode.
 * Dumps every mode the BIOS reports, highlighting 640x480x8 candidates.
 */

#include <stdio.h>
#include <string.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/nearptr.h>

int main(void)
{
    _go32_dpmi_seginfo ctrl_seg = { 0 };
    _go32_dpmi_seginfo mode_seg = { 0 };
    unsigned long      ctrl_lin, mode_lin, modes_lin;
    unsigned char     *ctrl, *mode;
    unsigned short    *modes;
    unsigned short     modes_off, modes_seg;
    __dpmi_regs        r;
    int                i, total = 0;

    printf("VESA diagnostic tool starting...\n");

    if (__djgpp_nearptr_enable() == 0) {
        printf("nearptr_enable FAILED.\n");
        return 1;
    }
    printf("nearptr base = 0x%08lX\n",
           (unsigned long)__djgpp_conventional_base);

    ctrl_seg.size = 32;
    if (_go32_dpmi_allocate_dos_memory(&ctrl_seg) != 0) {
        printf("allocate ctrl_seg FAILED.\n");
        return 1;
    }
    mode_seg.size = 16;
    if (_go32_dpmi_allocate_dos_memory(&mode_seg) != 0) {
        printf("allocate mode_seg FAILED.\n");
        return 1;
    }

    ctrl_lin = (unsigned long)ctrl_seg.rm_segment * 16UL;
    mode_lin = (unsigned long)mode_seg.rm_segment * 16UL;
    ctrl = (unsigned char *)(__djgpp_conventional_base + ctrl_lin);
    mode = (unsigned char *)(__djgpp_conventional_base + mode_lin);

    printf("ctrl blk at %04X:0000 (linear 0x%lX)\n",
           ctrl_seg.rm_segment, ctrl_lin);
    printf("mode blk at %04X:0000 (linear 0x%lX)\n",
           mode_seg.rm_segment, mode_lin);

    ctrl[0] = 'V'; ctrl[1] = 'B'; ctrl[2] = 'E'; ctrl[3] = '2';

    memset(&r, 0, sizeof(r));
    r.x.ax = 0x4F00;
    r.x.di = 0;
    r.x.es = ctrl_seg.rm_segment;
    __dpmi_int(0x10, &r);
    printf("INT 10 AX=4F00 -> AX=%04X\n", r.x.ax);

    if (r.x.ax != 0x004F) {
        printf("VBE not supported by this BIOS.\n");
        return 1;
    }

    printf("VBE signature: %.4s   version=%04X\n",
           ctrl, *(unsigned short *)(ctrl + 4));
    printf("Total memory (64K blocks): %u\n",
           *(unsigned short *)(ctrl + 18));

    modes_off = *(unsigned short *)(ctrl + 14);
    modes_seg = *(unsigned short *)(ctrl + 16);
    modes_lin = (unsigned long)modes_seg * 16UL + (unsigned long)modes_off;
    printf("Mode list at %04X:%04X (linear 0x%lX)\n",
           modes_seg, modes_off, modes_lin);

    modes = (unsigned short *)(__djgpp_conventional_base + modes_lin);

    printf("\n--- All modes reported by the BIOS ---\n");
    printf("  num  attr  res       bpp  pitch  LFB\n");

    for (i = 0; i < 1024; i++) {
        unsigned short m = modes[i];
        unsigned short xres, yres, pitch;
        unsigned char  attr, bpp;
        unsigned long  lfb;

        if (m == 0xFFFF) break;

        memset(&r, 0, sizeof(r));
        r.x.ax = 0x4F01;
        r.x.cx = m;
        r.x.di = 0;
        r.x.es = mode_seg.rm_segment;
        __dpmi_int(0x10, &r);
        if (r.x.ax != 0x004F) continue;

        attr  = mode[0x00];
        pitch = *(unsigned short *)(mode + 0x10);
        xres  = *(unsigned short *)(mode + 0x12);
        yres  = *(unsigned short *)(mode + 0x14);
        bpp   = mode[0x19];
        lfb   = *(unsigned long *)(mode + 0x28);

        /* Only print modes that are actually available. */
        if (!(attr & 0x01)) continue;

        printf("  %04X  %02X   %4ux%-4u  %2u   %4u   0x%08lX%s%s%s\n",
               m, attr, xres, yres, bpp, pitch, lfb,
               (attr & 0x08) ? " C" : "",
               (attr & 0x80) ? " L" : "",
               (xres == 640 && yres == 480 && bpp == 8) ? "  <== 640x480x8" : "");
        total++;
    }

    printf("\nTotal available modes: %d\n", total);
    printf("Done. No mode was set.\n");
    return 0;
}
