// Load ROMs directly from MAME zip sets (roms/kof98.zip + roms/neogeo.zip).
#pragma once
#include "rt.h"

struct RomSet {
    u8 *prom;   // 5 MiB decrypted 68k program image
    u8 *bios;   // 128 KiB sp-s2.sp1, word-swapped to big-endian
    u8 *sfix;   // 128 KiB
    u8 *s1;     // 128 KiB
    u8 *zoomy;  // 128 KiB (000-lo.lo)
    u8 *crom;   // 64 MiB, byte-interleaved pairs
    u8 *sm1;    // 128 KiB
    u8 *vrom;   // 16 MiB
    u8 *m1;     // 256 KiB
    size_t m1_sz;
};

// Returns 0 on success, negative on error (zip missing/corrupt/entry missing).
int romset_load_zip(const char *kof98_zip, const char *neogeo_zip, RomSet *out);
