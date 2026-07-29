// ROM loading directly from MAME zip sets: minimal zip reader (stored +
// deflate via a self-contained inflate), Neo Geo layout assembly, and the
// MAME prot_kof98 68k decryption. Mirrors tools/py/prepare_roms.py.
#include "romload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- CRC32 ----
static u32 g_crc_tab[256];
static int g_crc_ready;
static void crc_init() {
    if (g_crc_ready) return;
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_tab[i] = c;
    }
    g_crc_ready = 1;
}
static u32 crc32_of(const u8 *b, size_t n) {
    crc_init();
    u32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = g_crc_tab[(c ^ b[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ------------------------------------------------------------- inflate ----
// Small RFC 1951 (deflate) decoder written for this project.
struct Infl {
    const u8 *in; size_t in_len, in_pos;
    u32 bitbuf; int bitcnt;
    u8 *out; size_t out_len, out_pos;
};

static int inf_bit(Infl *s) {
    if (s->bitcnt == 0) {
        if (s->in_pos >= s->in_len) return -1;
        s->bitbuf = s->in[s->in_pos++];
        s->bitcnt = 8;
    }
    int b = (int)(s->bitbuf & 1);
    s->bitbuf >>= 1;
    s->bitcnt--;
    return b;
}
static int inf_bits(Infl *s, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        int b = inf_bit(s);
        if (b < 0) return -1;
        v |= b << i;
    }
    return v;
}

struct Huff {
    u16 count[16];      // number of codes for each bit length
    u16 symbol[288];    // symbols ordered by (length, symbol value)
};

// Build decoding tables from code lengths. 0 ok, <0 over-subscribed/invalid.
static int huff_build(Huff *h, const u8 *lengths, int n) {
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++) {
        if (lengths[i] > 15) return -1;
        h->count[lengths[i]]++;
    }
    h->count[0] = 0;    // zero-length means "no code", not a real length
    int left = 1;
    for (int len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }
    u16 offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (u16)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (lengths[i]) h->symbol[offs[lengths[i]]++] = (u16)i;
    return 0;
}

static int huff_decode(Infl *s, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        int b = inf_bit(s);
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static const u16 LEN_BASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258};
static const u8 LEN_EXT[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
    3,3,3,3,4,4,4,4,5,5,5,5,0};
static const u16 DIST_BASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const u8 DIST_EXT[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
    7,7,8,8,9,9,10,10,11,11,12,12,13,13};
static const u8 CLC_ORDER[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

static int inf_codes(Infl *s, const Huff *lit, const Huff *dist) {
    for (;;) {
        int sym = huff_decode(s, lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->out_pos >= s->out_len) return -1;
            s->out[s->out_pos++] = (u8)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int eb = inf_bits(s, LEN_EXT[sym]);
            if (eb < 0) return -1;
            int len = LEN_BASE[sym] + eb;
            int dsym = huff_decode(s, dist);
            if (dsym < 0 || dsym >= 30) return -1;
            eb = inf_bits(s, DIST_EXT[dsym]);
            if (eb < 0) return -1;
            size_t d = (size_t)DIST_BASE[dsym] + (size_t)eb;
            if (d > s->out_pos || s->out_pos + (size_t)len > s->out_len) return -1;
            for (int i = 0; i < len; i++) {
                s->out[s->out_pos] = s->out[s->out_pos - d];
                s->out_pos++;
            }
        }
    }
}

static void huff_fixed(Huff *lit, Huff *dist) {
    u8 lengths[288];
    for (int i = 0; i < 144; i++) lengths[i] = 8;
    for (int i = 144; i < 256; i++) lengths[i] = 9;
    for (int i = 256; i < 280; i++) lengths[i] = 7;
    for (int i = 280; i < 288; i++) lengths[i] = 8;
    huff_build(lit, lengths, 288);
    u8 dlen[30];
    for (int i = 0; i < 30; i++) dlen[i] = 5;
    huff_build(dist, dlen, 30);
}

static int inf_dynamic(Infl *s) {
    int hlit = inf_bits(s, 5);
    int hdist = inf_bits(s, 5);
    int hclen = inf_bits(s, 4);
    if (hlit < 0 || hdist < 0 || hclen < 0) return -1;
    hlit += 257; hdist += 1; hclen += 4;
    if (hlit > 286 || hdist > 30) return -1;
    u8 clen[19];
    memset(clen, 0, sizeof(clen));
    for (int i = 0; i < hclen; i++) {
        int v = inf_bits(s, 3);
        if (v < 0) return -1;
        clen[CLC_ORDER[i]] = (u8)v;
    }
    Huff clch;
    if (huff_build(&clch, clen, 19)) return -1;
    u8 lens[286 + 30];
    int n = 0, total = hlit + hdist;
    while (n < total) {
        int sym = huff_decode(s, &clch);
        if (sym < 0) return -1;
        if (sym < 16) {
            lens[n++] = (u8)sym;
        } else {
            int rep;
            u8 val = 0;
            if (sym == 16) {
                if (n == 0) return -1;
                rep = inf_bits(s, 2);
                if (rep < 0) return -1;
                rep += 3;
                val = lens[n - 1];
            } else if (sym == 17) {
                rep = inf_bits(s, 3);
                if (rep < 0) return -1;
                rep += 3;
            } else if (sym == 18) {
                rep = inf_bits(s, 7);
                if (rep < 0) return -1;
                rep += 11;
            } else {
                return -1;
            }
            if (n + rep > total) return -1;
            while (rep--) lens[n++] = val;
        }
    }
    Huff lit, dist;
    if (huff_build(&lit, lens, hlit)) return -1;
    if (huff_build(&dist, lens + hlit, hdist)) return -1;
    return inf_codes(s, &lit, &dist);
}

// Decompress a raw deflate stream into out[0..out_len). 0 ok, <0 error.
static int inflate_raw(const u8 *in, size_t in_len, u8 *out, size_t out_len) {
    Infl s;
    s.in = in; s.in_len = in_len; s.in_pos = 0;
    s.bitbuf = 0; s.bitcnt = 0;
    s.out = out; s.out_len = out_len; s.out_pos = 0;
    int last;
    do {
        last = inf_bits(&s, 1);
        int type = inf_bits(&s, 2);
        if (last < 0 || type < 0) return -1;
        if (type == 0) {
            s.bitcnt = 0;   // skip to byte boundary
            if (s.in_pos + 4 > s.in_len) return -1;
            int len = s.in[s.in_pos] | (s.in[s.in_pos + 1] << 8);
            int nlen = s.in[s.in_pos + 2] | (s.in[s.in_pos + 3] << 8);
            s.in_pos += 4;
            if (len != (nlen ^ 0xFFFF)) return -1;
            if (s.in_pos + (size_t)len > s.in_len || s.out_pos + (size_t)len > s.out_len) return -1;
            memcpy(s.out + s.out_pos, s.in + s.in_pos, len);
            s.in_pos += len;
            s.out_pos += len;
        } else if (type == 1) {
            Huff lit, dist;
            huff_fixed(&lit, &dist);
            if (inf_codes(&s, &lit, &dist)) return -1;
        } else if (type == 2) {
            if (inf_dynamic(&s)) return -1;
        } else {
            return -1;
        }
    } while (!last);
    return s.out_pos == s.out_len ? 0 : -1;
}

// ----------------------------------------------------------------- zip ----
struct ZipEntry {
    char name[128];
    u32 method, crc, comp, uncomp, local_off;
};
struct Zip {
    u8 *data;
    size_t size;
    ZipEntry *entries;
    int count;
};

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static void zip_close(Zip *z) {
    free(z->data);
    free(z->entries);
    z->data = NULL; z->entries = NULL; z->count = 0;
}

static int zip_open(const char *path, Zip *z) {
    z->data = NULL; z->entries = NULL; z->count = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 22) { fclose(f); return -2; }
    z->data = (u8 *)malloc(n);
    z->size = (size_t)n;
    if (fread(z->data, 1, n, f) != (size_t)n) { fclose(f); zip_close(z); return -2; }
    fclose(f);
    // locate end of central directory
    size_t eocd = z->size;
    int found = 0;
    for (size_t i = z->size - 22;; i--) {
        if (z->data[i] == 'P' && z->data[i + 1] == 'K' && z->data[i + 2] == 5 && z->data[i + 3] == 6) {
            eocd = i; found = 1; break;
        }
        if (i == 0) break;
    }
    if (!found) { zip_close(z); return -2; }
    int count = rd16(z->data + eocd + 10);
    size_t cd = rd32(z->data + eocd + 16);
    z->entries = (ZipEntry *)calloc(count, sizeof(ZipEntry));
    for (int i = 0; i < count; i++) {
        if (cd + 46 > z->size || rd32(z->data + cd) != 0x02014B50) { zip_close(z); return -2; }
        const u8 *e = z->data + cd;
        ZipEntry *t = &z->entries[z->count++];
        t->method = rd16(e + 10);
        t->crc = rd32(e + 16);
        t->comp = rd32(e + 20);
        t->uncomp = rd32(e + 24);
        t->local_off = rd32(e + 42);
        int nl = rd16(e + 28), xl = rd16(e + 30), cl = rd16(e + 32);
        if (nl > 127) nl = 127;
        memcpy(t->name, e + 46, nl);
        t->name[nl] = 0;
        cd += 46 + rd16(e + 28) + xl + cl;
    }
    return 0;
}

// Extract one entry (CRC-verified). Returns malloc'd buffer or NULL.
static u8 *zip_read(Zip *z, const char *name, size_t *out_len) {
    for (int i = 0; i < z->count; i++) {
        ZipEntry *t = &z->entries[i];
        if (strcmp(t->name, name) != 0) continue;
        if ((size_t)t->local_off + 30 > z->size) return NULL;
        const u8 *lh = z->data + t->local_off;
        if (rd32(lh) != 0x04034B50) return NULL;
        const u8 *src = lh + 30 + rd16(lh + 26) + rd16(lh + 28);
        if (src + t->comp > z->data + z->size) return NULL;
        u8 *out = (u8 *)malloc(t->uncomp ? t->uncomp : 1);
        int ok;
        if (t->method == 0) {
            if (t->comp != t->uncomp) { free(out); return NULL; }
            memcpy(out, src, t->uncomp);
            ok = 0;
        } else if (t->method == 8) {
            ok = inflate_raw(src, t->comp, out, t->uncomp);
        } else {
            free(out);
            return NULL;
        }
        if (ok || crc32_of(out, t->uncomp) != t->crc) {
            fprintf(stderr, "zip: bad entry %s in archive\n", name);
            free(out);
            return NULL;
        }
        *out_len = t->uncomp;
        return out;
    }
    return NULL;
}

// ------------------------------------------------------- Neo Geo layout ----
static void swap16_into(u8 *dst, const u8 *src, size_t n) {
    for (size_t i = 0; i < n; i += 2) {
        dst[i] = src[i + 1];
        dst[i + 1] = src[i];
    }
}

// MAME prot_kof98 decrypt_68k, on the big-endian cpurom buffer (p1 2M + p2 4M).
static void kof98_decrypt_68k(u8 *src) {
    static const u32 sec[8] = {0x000000, 0x100000, 0x000004, 0x100004,
                               0x10000a, 0x00000a, 0x10000e, 0x00000e};
    static const u32 pos[4] = {0x000, 0x004, 0x00a, 0x00e};
    u8 *dst = (u8 *)malloc(0x200000);
    memcpy(dst, src, 0x200000);
    for (u32 i = 0x800; i < 0x100000; i += 0x200) {
        for (u32 j = 0; j < 0x100; j += 0x10) {
            for (u32 k = 0; k < 16; k += 2) {
                u32 s = sec[k / 2];
                src[i + j + k] = dst[i + j + s + 0x100];
                src[i + j + k + 1] = dst[i + j + s + 0x101];
                src[i + j + k + 0x100] = dst[i + j + s];
                src[i + j + k + 0x101] = dst[i + j + s + 1];
            }
            if (i >= 0x80000 && i < 0xC0000) {
                for (int k = 0; k < 4; k++) {
                    u32 p = pos[k];
                    src[i + j + p] = dst[i + j + p];
                    src[i + j + p + 1] = dst[i + j + p + 1];
                    src[i + j + p + 0x100] = dst[i + j + p + 0x100];
                    src[i + j + p + 0x101] = dst[i + j + p + 0x101];
                }
            } else if (i >= 0xC0000) {
                for (int k = 0; k < 4; k++) {
                    u32 p = pos[k];
                    src[i + j + p] = dst[i + j + p + 0x100];
                    src[i + j + p + 1] = dst[i + j + p + 0x101];
                    src[i + j + p + 0x100] = dst[i + j + p];
                    src[i + j + p + 0x101] = dst[i + j + p + 1];
                }
            }
        }
        src[i + 0x000] = dst[i + 0x000000];
        src[i + 0x001] = dst[i + 0x000001];
        src[i + 0x002] = dst[i + 0x100000];
        src[i + 0x003] = dst[i + 0x100001];
        src[i + 0x100] = dst[i + 0x000100];
        src[i + 0x101] = dst[i + 0x000101];
        src[i + 0x102] = dst[i + 0x100100];
        src[i + 0x103] = dst[i + 0x100101];
    }
    memmove(src + 0x100000, src + 0x200000, 0x400000);
    free(dst);
}

static void dump_if_asked(const char *name, const u8 *b, size_t n) {
    if (!getenv("KOF98_DUMPROM")) return;
    FILE *f = fopen(name, "wb");
    if (f) { fwrite(b, 1, n, f); fclose(f); }
}

int romset_load_zip(const char *kof98_zip, const char *neogeo_zip, RomSet *out) {
    Zip kof, bio;
    memset(out, 0, sizeof(*out));
    if (zip_open(kof98_zip, &kof)) return -1;
    if (zip_open(neogeo_zip, &bio)) { zip_close(&kof); return -1; }
    size_t n;
    int rc = -3;

    // ---- program: swap16(p1) + swap16(p2), decrypt, keep first 5 MiB ----
    u8 *p1 = zip_read(&kof, "242-p1.p1", &n);
    u8 *p2 = zip_read(&kof, "242-p2.sp2", &n);
    if (!p1 || !p2) goto fail;
    {
        u8 *cpurom = (u8 *)malloc(0x600000);
        swap16_into(cpurom, p1, 0x200000);
        swap16_into(cpurom + 0x200000, p2, 0x400000);
        free(p1); free(p2);
        kof98_decrypt_68k(cpurom);
        out->prom = (u8 *)malloc(0x500000);
        memcpy(out->prom, cpurom, 0x500000);
        free(cpurom);
    }
    // ---- BIOS: swap16(sp-s2.sp1) ----
    {
        u8 *sp = zip_read(&bio, "sp-s2.sp1", &n);
        if (!sp) goto fail;
        out->bios = (u8 *)malloc(0x20000);
        swap16_into(out->bios, sp, 0x20000);
        free(sp);
    }
    // ---- raw members ----
    if (!(out->sfix = zip_read(&bio, "sfix.sfix", &n))) goto fail;
    if (!(out->zoomy = zip_read(&bio, "000-lo.lo", &n))) goto fail;
    if (!(out->sm1 = zip_read(&bio, "sm1.sm1", &n))) goto fail;
    if (!(out->s1 = zip_read(&kof, "242-s1.s1", &n))) goto fail;
    if (!(out->m1 = zip_read(&kof, "242-m1.m1", &out->m1_sz))) goto fail;
    // ---- sprites: byte-interleave (c1,c2),(c3,c4),(c5,c6),(c7,c8) ----
    {
        static const char *cn[8] = {"242-c1.c1", "242-c2.c2", "242-c3.c3", "242-c4.c4",
                                    "242-c5.c5", "242-c6.c6", "242-c7.c7", "242-c8.c8"};
        out->crom = (u8 *)malloc(0x4000000);
        u8 *d = out->crom;
        for (int p = 0; p < 8; p += 2) {
            size_t na, nb;
            u8 *a = zip_read(&kof, cn[p], &na);
            u8 *b = zip_read(&kof, cn[p + 1], &nb);
            if (!a || !b || na != 0x800000 || nb != 0x800000) { free(a); free(b); goto fail; }
            for (size_t i = 0; i < 0x800000; i++) {
                d[2 * i] = a[i];
                d[2 * i + 1] = b[i];
            }
            free(a); free(b);
            d += 0x1000000;
        }
    }
    // ---- ADPCM: v1|v2|v3|v4 ----
    {
        static const char *vn[4] = {"242-v1.v1", "242-v2.v2", "242-v3.v3", "242-v4.v4"};
        out->vrom = (u8 *)malloc(0x1000000);
        u8 *d = out->vrom;
        for (int i = 0; i < 4; i++) {
            size_t nv;
            u8 *v = zip_read(&kof, vn[i], &nv);
            if (!v || nv != 0x400000) { free(v); goto fail; }
            memcpy(d, v, 0x400000);
            free(v);
            d += 0x400000;
        }
    }
    rc = 0;
    dump_if_asked("z_prom.bin", out->prom, 0x500000);
    dump_if_asked("z_bios.bin", out->bios, 0x20000);
    dump_if_asked("z_sfix.bin", out->sfix, 0x20000);
    dump_if_asked("z_s1.bin", out->s1, 0x20000);
    dump_if_asked("z_zoomy.bin", out->zoomy, 0x20000);
    dump_if_asked("z_sm1.bin", out->sm1, 0x20000);
    dump_if_asked("z_m1.bin", out->m1, out->m1_sz);
    dump_if_asked("z_crom.bin", out->crom, 0x4000000);
    dump_if_asked("z_vrom.bin", out->vrom, 0x1000000);
fail:
    zip_close(&kof);
    zip_close(&bio);
    return rc;
}
