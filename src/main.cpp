// Win32 platform layer: window, input, frame pacing. Also headless debug mode.
#include "rt.h"
#include <windows.h>

extern u16 g_vram_offset;
void bram_save();

static HWND g_hwnd;
static BITMAPINFO g_bmi;
static int g_quit;
static int g_key[256];

// ---- configurable keys (kof98.ini next to the exe) ----
static int k_p1[8], k_p2[8], k_p1_start, k_p2_start, k_p1_coin, k_p2_coin,
           k_svc_coin, k_p1_sel, k_p2_sel, k_test, k_quit;
struct Bind { const char *name; const char *def; int *slot; };
static Bind g_binds[] = {
    {"p1_up", "UP", &k_p1[0]}, {"p1_down", "DOWN", &k_p1[1]},
    {"p1_left", "LEFT", &k_p1[2]}, {"p1_right", "RIGHT", &k_p1[3]},
    {"p1_a", "Z", &k_p1[4]}, {"p1_b", "X", &k_p1[5]}, {"p1_c", "C", &k_p1[6]}, {"p1_d", "V", &k_p1[7]},
    {"p1_start", "1", &k_p1_start}, {"p1_select", "3", &k_p1_sel}, {"p1_coin", "5", &k_p1_coin},
    {"p2_up", "I", &k_p2[0]}, {"p2_down", "K", &k_p2[1]},
    {"p2_left", "J", &k_p2[2]}, {"p2_right", "L", &k_p2[3]},
    {"p2_a", "NUMPAD1", &k_p2[4]}, {"p2_b", "NUMPAD2", &k_p2[5]},
    {"p2_c", "NUMPAD3", &k_p2[6]}, {"p2_d", "NUMPAD4", &k_p2[7]},
    {"p2_start", "2", &k_p2_start}, {"p2_select", "4", &k_p2_sel}, {"p2_coin", "6", &k_p2_coin},
    {"service_coin", "7", &k_svc_coin}, {"test", "F2", &k_test}, {"quit", "ESCAPE", &k_quit},
    {NULL, NULL, NULL}
};

static int vk_from_name(const char *s) {
    if (!s[0]) return 0;
    if (!s[1]) { char c = s[0]; if (c >= 'a' && c <= 'z') c -= 'a' - 'A'; return (unsigned char)c; }
    static const struct { const char *n; int vk; } t[] = {
        {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
        {"SPACE", VK_SPACE}, {"ENTER", VK_RETURN}, {"RETURN", VK_RETURN},
        {"SHIFT", VK_SHIFT}, {"LSHIFT", VK_LSHIFT}, {"RSHIFT", VK_RSHIFT},
        {"CTRL", VK_CONTROL}, {"CONTROL", VK_CONTROL}, {"LCTRL", VK_LCONTROL}, {"RCTRL", VK_RCONTROL},
        {"ALT", VK_MENU}, {"TAB", VK_TAB}, {"ESCAPE", VK_ESCAPE}, {"ESC", VK_ESCAPE},
        {"BACKSPACE", VK_BACK}, {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE},
        {"HOME", VK_HOME}, {"END", VK_END}, {"PGUP", VK_PRIOR}, {"PGDN", VK_NEXT},
    };
    for (int i = 0; i < (int)(sizeof(t) / sizeof(t[0])); i++)
        if (!_stricmp(s, t[i].n)) return t[i].vk;
    if (!_strnicmp(s, "NUMPAD", 6) && s[6] >= '0' && s[6] <= '9' && !s[7]) return VK_NUMPAD0 + (s[6] - '0');
    if ((s[0] == 'F' || s[0] == 'f')) { int n = atoi(s + 1); if (n >= 1 && n <= 12) return VK_F1 + n - 1; }
    return 0;
}

static void load_keyconfig() {
    for (Bind *b = g_binds; b->name; b++) *b->slot = vk_from_name(b->def);
    FILE *f = fopen("kof98.ini", "rb");
    if (!f) {   // first run: write defaults for the user to edit
        f = fopen("kof98.ini", "wb");
        if (f) {
            fprintf(f, "; KOF98 key config. Values: A-Z, 0-9, UP/DOWN/LEFT/RIGHT, NUMPAD0-9,\n"
                       "; F1-F12, SPACE, ENTER, SHIFT, CTRL, ALT, TAB, ESCAPE, HOME, END, PGUP, PGDN\n"
                       "[keys]\n");
            for (Bind *b = g_binds; b->name; b++) fprintf(f, "%s=%s\n", b->name, b->def);
            fclose(f);
        }
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        if (*s == ';' || *s == '#' || *s == '[' || *s == '\r' || *s == '\n' || !*s) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *ne = eq; while (ne > s && (ne[-1] == ' ' || ne[-1] == '\t')) --ne; *ne = 0;
        char *val = eq + 1; while (*val == ' ' || *val == '\t') val++;
        char *ve = val + strlen(val);
        while (ve > val && (ve[-1] == '\n' || ve[-1] == '\r' || ve[-1] == ' ' || ve[-1] == '\t')) --ve;
        *ve = 0;
        for (Bind *b = g_binds; b->name; b++)
            if (!_stricmp(b->name, s)) { int vk = vk_from_name(val); if (vk) *b->slot = vk; break; }
    }
    fclose(f);
}

static void poll_input() {
    g_in_p1 = 0xFF;
    for (int i = 0; i < 8; i++) if (g_key[k_p1[i]]) g_in_p1 &= ~(u8)(1 << i);
    g_in_p2 = 0xFF;
    for (int i = 0; i < 8; i++) if (g_key[k_p2[i]]) g_in_p2 &= ~(u8)(1 << i);
    g_in_start = 0;
    if (g_key[k_p1_start]) g_in_start |= 1;
    if (g_key[k_p2_start]) g_in_start |= 4;
    g_in_coin = 0xFF;
    if (g_key[k_p1_coin]) g_in_coin &= ~0x01;
    if (g_key[k_p2_coin]) g_in_coin &= ~0x02;
    if (g_key[k_svc_coin]) g_in_coin &= ~0x04;
    g_in_select = 0xFF;
    if (g_key[k_p1_sel]) g_in_select &= ~0x01;
    if (g_key[k_p2_sel]) g_in_select &= ~0x04;
    g_in_service = g_key[k_test] ? 0 : 1;
    if (g_key[k_quit]) g_quit = 1;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { g_quit = 1; PostQuitMessage(0); return 0; }
    if (m == WM_KEYDOWN) { g_key[w & 0xFF] = 1; return 0; }
    if (m == WM_KEYUP) { g_key[w & 0xFF] = 0; return 0; }
    return DefWindowProc(h, m, w, l);
}

// ---- waveOut audio ----
#define AU_BUFS 4
#define AU_FRAMES 735           // 735*4 = 2940 frames ≈ 66ms total
static HWAVEOUT g_hwo;
static WAVEHDR g_ahdr[AU_BUFS];
static s16 g_abuf[AU_BUFS][AU_FRAMES * 2];
static void audio_init() {
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 44100;
    wf.nAvgBytesPerSec = 44100 * 4;
    wf.nBlockAlign = 4;
    wf.wBitsPerSample = 16;
    if (waveOutOpen(&g_hwo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        g_hwo = NULL;
        return;
    }
    for (int i = 0; i < AU_BUFS; i++) {
        memset(&g_ahdr[i], 0, sizeof(WAVEHDR));
        g_ahdr[i].lpData = (LPSTR)g_abuf[i];
        g_ahdr[i].dwBufferLength = sizeof(g_abuf[i]);
        waveOutPrepareHeader(g_hwo, &g_ahdr[i], sizeof(WAVEHDR));
    }
}
static void audio_pump() {
    if (!g_hwo) return;
    for (int i = 0; i < AU_BUFS; i++) {
        if (g_ahdr[i].dwFlags & WHDR_INQUEUE) continue;
        u32 got = audio_ring_pop(g_abuf[i], AU_FRAMES);
        if (got < AU_FRAMES) memset(g_abuf[i] + got * 2, 0, (AU_FRAMES - got) * 4);
        waveOutWrite(g_hwo, &g_ahdr[i], sizeof(WAVEHDR));
    }
}

static void save_bmp(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int w = SCREEN_W, h = SCREEN_H;
    int row = w * 3, pad = (4 - row % 4) % 4, imgsz = (row + pad) * h;
    u8 hdr[54] = {'B', 'M'};
    u32 fsz = 54 + imgsz;
    hdr[2] = fsz & 0xFF; hdr[3] = (fsz >> 8) & 0xFF; hdr[4] = (fsz >> 16) & 0xFF; hdr[5] = (fsz >> 24) & 0xFF;
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = w & 0xFF; hdr[19] = (w >> 8) & 0xFF;
    hdr[22] = h & 0xFF; hdr[23] = (h >> 8) & 0xFF;
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    u8 *line = (u8 *)malloc(row + pad);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            u32 p = g_fb[y * w + x];
            line[x * 3 + 0] = p & 0xFF; line[x * 3 + 1] = (p >> 8) & 0xFF; line[x * 3 + 2] = (p >> 16) & 0xFF;
        }
        memset(line + row, 0, pad);
        fwrite(line, 1, row + pad, f);
    }
    free(line);
    fclose(f);
}

static int g_cur_frame;
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    FILE *f = fopen("crash.txt", "w");
    if (f) {
        fprintf(f, "CRASH frame=%d code=%08x addr=%p pc=%06x cyc=%llu rip=%llx\n",
                g_cur_frame, (unsigned)ep->ExceptionRecord->ExceptionCode,
                ep->ExceptionRecord->ExceptionAddress, cpu.pc, (unsigned long long)cpu.cyc,
                (unsigned long long)ep->ContextRecord->Rip);
        fprintf(f, "ntrace=%d vpos=%d\n", g_ntrace, g_vpos);
        extern u32 g_pcring[1024]; extern u32 g_pcring_pos;
        fprintf(f, "last pcs:");
        for (int k = 40; k >= 1; k--)
            fprintf(f, " %06x", g_pcring[(g_pcring_pos - k) & 1023]);
        fprintf(f, "\n");
        fprintf(f, "D: %08x %08x %08x %08x %08x %08x %08x %08x\nA: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                cpu.D[0], cpu.D[1], cpu.D[2], cpu.D[3], cpu.D[4], cpu.D[5], cpu.D[6], cpu.D[7],
                cpu.A[0], cpu.A[1], cpu.A[2], cpu.A[3], cpu.A[4], cpu.A[5], cpu.A[6], cpu.A[7]);
        fclose(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char **argv) {
    SetUnhandledExceptionFilter(crash_filter);
    // resolve all data paths relative to the exe location (roms in rom\
    // next to the exe): run with CWD = exe directory
    {
        char exedir[MAX_PATH];
        GetModuleFileNameA(NULL, exedir, MAX_PATH);
        char *p = strrchr(exedir, '\\');
        if (p) { *(p + 1) = 0; SetCurrentDirectoryA(exedir); }
    }
    load_keyconfig();
    if (getenv("KOF98_FMTEST")) {
        void ym2610_selftest(const char *path);
        ym2610_selftest("fmtest.raw");
        return 0;
    }
    machine_init();

    const char *hl = getenv("KOF98_HEADLESS");
    if (hl) {
        int frames = atoi(hl);
        int coin_at = getenv("KOF98_COIN_AT") ? atoi(getenv("KOF98_COIN_AT")) : -1;
        int start_at = getenv("KOF98_START_AT") ? atoi(getenv("KOF98_START_AT")) : -1;
        FILE *pf = getenv("KOF98_PROGRESS") ? fopen("progress.txt", "w") : NULL;
        LARGE_INTEGER pfq, pt0, pt1;
        QueryPerformanceFrequency(&pfq);
        QueryPerformanceCounter(&pt0);
        for (int i = 0; i < frames; i++) {
            g_cur_frame = i;
            if (pf) { fprintf(pf, "frame %d pc=%06x cyc=%llu tick=%lu\n", i, cpu.pc, (unsigned long long)cpu.cyc, (unsigned long)GetTickCount()); fflush(pf); }
            // scripted input pulses (8 frames each)
            g_in_coin = 0xFF; g_in_start = 0;
            if (coin_at >= 0 && i >= coin_at && i < coin_at + 8) g_in_coin &= ~0x01;
            if (start_at >= 0 && i >= start_at && i < start_at + 8) g_in_start |= 1;
            machine_frame();
            if ((i & 255) == 0 && !HeapValidate(GetProcessHeap(), 0, NULL)) {
                extern u32 g_pcring[1024]; extern u32 g_pcring_pos;
                fprintf(stderr, "HEAPBAD frame=%d pc=%06x\nlast pcs:", i, cpu.pc);
                for (int k = 40; k >= 1; k--)
                    fprintf(stderr, " %06x", g_pcring[(g_pcring_pos - k) & 1023]);
                fprintf(stderr, "\n");
                return 7;
            }
            if (getenv("KOF98_SAMPLE") && (i % 5 == 0))
                printf("f%04d pc=%06x irqlv=%d vb=%d iml=%d fee4=%02x a6=%06x d6=%08x\n", i, cpu.pc, g_irq_level, g_irq_vblank, cpu.iml, g_wram[0xfee4] & 0xFF, cpu.A[6], cpu.D[6]);
            if (getenv("KOF98_SHOTS") && i && (i % 200 == 0)) {
                char nm[64]; snprintf(nm, 64, "shot_%04d.bmp", i);
                save_bmp(nm);
            }
        }
        save_bmp("frame.bmp");
        {
            FILE *df = fopen("dump.txt", "w");
            if (df) {
                fprintf(df, "pc=%06x vram_off=%04x palbank=%d shadow=%d cartfix=%d\n",
                        cpu.pc, g_vram_offset, g_palette_bank, g_screen_shadow, g_use_cart_audio);
                fprintf(df, "pal[0..7]:");
                for (int i = 0; i < 8; i++) fprintf(df, " %04x", g_palram[i]);
                fprintf(df, "\npal[0xff8..0xfff]:");
                for (int i = 0xff8; i < 0x1000; i++) fprintf(df, " %04x", g_palram[i]);
                fprintf(df, "\nfixmap rows 0-3:\n");
                FILE *vf = fopen("vram.bin", "wb");
                if (vf) { fwrite(g_vram, 2, 0x10000, vf); fclose(vf); }
                for (int r = 0; r < 4; r++) {
                    for (int c = 0; c < 40; c++) fprintf(df, "%04x ", g_vram[0x7000 + r + c * 0x20]);
                    fprintf(df, "\n");
                }
                fprintf(df, "wram fd00-fd10:");
                for (int i = 0xfd00; i < 0xfd10; i += 2) fprintf(df, " %02x%02x", g_wram[i], g_wram[i + 1]);
                fprintf(df, "\nwram ff00-ff20:");
                for (int i = 0xff00; i < 0xff20; i += 2) fprintf(df, " %02x%02x", g_wram[i], g_wram[i + 1]);
                fprintf(df, "\n");
                fclose(df);
            }
        }
        if (getenv("KOF98_COV")) cov_dump();
        bram_save();
        if (g_trace) fclose(g_trace);
        QueryPerformanceCounter(&pt1);
        double elms = (double)(pt1.QuadPart - pt0.QuadPart) * 1000.0 / (double)pfq.QuadPart;
        printf("perf: %d frames in %.1f ms = %.1f fps (%.3f ms/frame)\n",
               frames, elms, elms > 0 ? frames * 1000.0 / elms : 0.0, frames ? elms / frames : 0.0);
        extern u64 g_audio_nonzero, g_audio_total, g_audio_clip;
        printf("headless done: %d frames, cyc=%llu pc=%06x z80cyc=%llu z80pc=%04x z80a=%02x audio=%llu/%llu clip=%llu\n", frames, (unsigned long long)cpu.cyc, cpu.pc, (unsigned long long)g_z80_cyc, z80_get_pc(), z80_get_a(), g_audio_nonzero, g_audio_total, g_audio_clip);
        return 0;
    }

    WNDCLASSA wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "kof98native";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    g_hwnd = CreateWindowExA(0, "kof98native", "KOF98 native",
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, 960, 720,
                             NULL, NULL, wc.hInstance, NULL);
    memset(&g_bmi, 0, sizeof(g_bmi));
    g_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_bmi.bmiHeader.biWidth = SCREEN_W;
    g_bmi.bmiHeader.biHeight = -SCREEN_H;
    g_bmi.bmiHeader.biPlanes = 1;
    g_bmi.bmiHeader.biBitCount = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;

    timeBeginPeriod(1);
    audio_init();
    LARGE_INTEGER freq, last;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    const double frame_ms = 1000.0 / (6000000.0 / 384.0 / 264.0);
    int win_frames = getenv("KOF98_WINFRAMES") ? atoi(getenv("KOF98_WINFRAMES")) : 0;
    int frames_run = 0;

    while (!g_quit) {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        poll_input();
        machine_frame();
        audio_pump();
        if (win_frames && ++frames_run >= win_frames) break;
        HDC dc = GetDC(g_hwnd);
        RECT rc; GetClientRect(g_hwnd, &rc);
        StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, SCREEN_W, SCREEN_H, g_fb, &g_bmi, DIB_RGB_COLORS, SRCCOPY);
        ReleaseDC(g_hwnd, dc);

        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double ms = (double)(now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart;
        if (ms < frame_ms) {                    // pace to ~59.2 fps
            double wait = frame_ms - ms;
            if (wait > 2.0) Sleep((DWORD)(wait - 1.0));
            do { QueryPerformanceCounter(&now); }
            while ((double)(now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart < frame_ms);
        }
        last = now;
    }
    timeEndPeriod(1);
    if (getenv("KOF98_COV")) cov_dump();
    bram_save();
    if (g_trace) fclose(g_trace);
    return 0;
}
