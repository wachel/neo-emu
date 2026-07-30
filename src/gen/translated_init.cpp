// AUTO-GENERATED
#include "rt.h"
void seg_00();
void seg_01();
void seg_02();
void seg_03();
void seg_04();
void seg_06();
void seg_08();
void seg_09();
void seg_0a();
void seg_0f();
void translated_init() {
    g_segtab[0x00] = seg_00;
    g_segtab[0x01] = seg_01;
    g_segtab[0x02] = seg_02;
    g_segtab[0x03] = seg_03;
    g_segtab[0x04] = seg_04;
    g_segtab[0x06] = seg_06;
    g_segtab[0x08] = seg_08;
    g_segtab[0x09] = seg_09;
    g_segtab[0x0a] = seg_0a;
    g_segtab[0x0f] = seg_0f;
}
