#ifndef _BS_DOS_RENDERER_H_
#define _BS_DOS_RENDERER_H_

#include "common.h"
#include "renderer.h"

// Software renderer targeting DOS VGA/VESA 640x480x8 (VESA mode 0x101,
// banked). Uses a fixed 3-3-2 RGB palette. Texture pages (TXTR) are
// decoded on first use via ImageDecoder_decodeToRgba and cached in
// linear RAM.
//
// Intended for FreeDOS + HDPMI32I as the DPMI host.
Renderer* DOSRenderer_create(void);

#endif /* _BS_DOS_RENDERER_H_ */
