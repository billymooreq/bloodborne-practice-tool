/* meta.c: what a slot shows in the overlay: the game's own values at F5 (place, play time, level, Blood
 * Echoes, insight, HP, position), a thumbnail of the screen, the slot's name. Kept in slotNN\info.ini
 * and slotNN\thumb.bmp next to the slot's data (written after the slot itself, read by the overlay).
 *
 * Game data (CUSA03173 1.09, checked in the running game):
 *   [0x805543ED0] -> +0 PlayerIns; PlayerIns+0x3f8 u32 map id (AA<<24 | BB<<16 | CC<<8 | DD = mAA_BB_CC_DD)
 *   GameDataMan [0x80553B130]: +0x94 u32 play time in ms; +8 PlayerGameData (= PlayerIns+0x3c0):
 *     +0x14 HP, +0x18 max HP, +0x84 insight, +0x90 level, +0x94 Blood Echoes
 *   PlayerIns+0x3B0 -> +0x68 physics module: position float[3] at +0x1E0
 * Place names: the game's English place-name text (msg/engus/item.msgbnd.dcx, 地名.fmg) is keyed by
 * AA*100 + BB*10 of the map id; sub-areas of one map (Iosefka's Clinic, Lecture Building...) share the
 * map's name here. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define COBJMACROS
#include <d3d11.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "meta.h"
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "user32.lib")

static const struct { uint8_t aa, bb; const char *name; } k_places[] = {
    {21, 0, "Hunter's Dream"}, {21, 1, "Abandoned Old Workshop"}, {22, 0, "Hemwick Charnel Lane"}, {23, 0, "Old Yharnam"},
    {24, 0, "Cathedral Ward"}, {24, 1, "Central Yharnam"}, {24, 2, "Upper Cathedral Ward"}, {25, 0, "Forsaken Cainhurst Castle"},
    {26, 0, "Nightmare of Mensis"}, {27, 0, "Forbidden Woods"}, {28, 0, "Yahar'gul, Unseen Village"}, {32, 0, "Byrgenwerth"},
    {33, 0, "Nightmare Frontier"}, {34, 0, "Hunter's Nightmare"}, {35, 0, "Research Hall"}, {36, 0, "Fishing Hamlet"},
};
const char *meta_place(uint32_t map_id) {
    uint8_t aa = (uint8_t)(map_id >> 24), bb = (uint8_t)(map_id >> 16);
    for (size_t i = 0; i < sizeof k_places / sizeof *k_places; ++i) if (k_places[i].aa == aa && k_places[i].bb == bb) return k_places[i].name;
    for (size_t i = 0; i < sizeof k_places / sizeof *k_places; ++i) if (k_places[i].aa == aa) return k_places[i].name;
    return aa == 29 ? "Chalice Dungeon" : "";
}

static uint64_t rq(uint64_t a) { return *(volatile uint64_t *)a; }
int meta_read_game(SlotMeta *m) {
    memset(m, 0, sizeof *m);
    __try {
        uint64_t A = rq(0x805543ED0ull), ins = A ? rq(A) : 0, gdm = rq(0x80553B130ull);
        if (!ins || !gdm) return 0;
        uint64_t pgd = rq(gdm + 8);
        m->map_id = *(volatile uint32_t *)(ins + 0x3f8);
        m->play_time_ms = *(volatile uint32_t *)(gdm + 0x94);
        if (pgd) {
            m->hp = *(volatile int32_t *)(pgd + 0x14); m->hp_max = *(volatile int32_t *)(pgd + 0x18);
            m->insight = *(volatile int32_t *)(pgd + 0x84); m->level = *(volatile int32_t *)(pgd + 0x90);
            m->echoes = *(volatile uint32_t *)(pgd + 0x94);
            wchar_t nm[17] = {0}; memcpy(nm, (const void *)(pgd + 0xA8), 32);   /* character name, wchar[16] */
            WideCharToMultiByte(CP_UTF8, 0, nm, -1, m->hunter, sizeof m->hunter, NULL, NULL);
        }
        uint64_t phys = rq(rq(ins + 0x3B0) + 0x68);
        memcpy(m->pos, (void *)(phys + 0x1E0), 12);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    strncpy_s(m->location, sizeof m->location, meta_place(m->map_id), _TRUNCATE);
    _snprintf_s(m->map, sizeof m->map, _TRUNCATE, "m%02u_%02u", (m->map_id >> 24) & 0xff, (m->map_id >> 16) & 0xff);
    SYSTEMTIME t; GetLocalTime(&t);
    _snprintf_s(m->created, sizeof m->created, _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return 1;
}
void meta_default_name(const SlotMeta *m, char *out, size_t n) {
    _snprintf_s(out, n, _TRUNCATE, "%s %.5s", m->location[0] ? m->location : m->map, m->created + 11);
}

/* ---- thumbnail: the game window as it is on screen, only when it is the foreground window (a save from a
 * test script or with another window on top gets none). The port presents with flip-model independent
 * flip: GDI reads of the screen come out black, so the frame comes from DXGI desktop duplication of the
 * monitor (asked for 8-bit BGRA, HDR is converted by the system), cropped to the window, box-filtered. */
/* the device and output stay from one capture to the next (making them took ~0.2 s); a duplication is per capture */
static HMONITOR g_mon; static IDXGIOutput *g_out; static ID3D11Device *g_dev; static ID3D11DeviceContext *g_ctx;
static int dup_capture(HWND w, const RECT *scr, uint8_t *bgr) {
    HMONITOR mon = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
    IDXGIFactory1 *fac = NULL; IDXGIAdapter1 *ad = NULL; IDXGIOutput *out = NULL; IDXGIOutput1 *o1 = NULL; IDXGIOutput5 *o5 = NULL;
    ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL; IDXGIOutputDuplication *dup = NULL;
    IDXGIResource *res = NULL; ID3D11Texture2D *tex = NULL, *stg = NULL;
    DXGI_OUTPUT_DESC od; int ok = 0, acquired = 0;
    if (g_dev && g_mon == mon) { out = g_out; dev = g_dev; ctx = g_ctx; IDXGIOutput_GetDesc(out, &od); goto have_device; }
    if (g_dev) { IDXGIOutput_Release(g_out); ID3D11DeviceContext_Release(g_ctx); ID3D11Device_Release(g_dev); g_dev = NULL; g_out = NULL; g_ctx = NULL; }
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&fac))) return 0;
    for (UINT i = 0; !out && IDXGIFactory1_EnumAdapters1(fac, i, &ad) == S_OK; ++i) {
        for (UINT j = 0; IDXGIAdapter1_EnumOutputs(ad, j, &out) == S_OK; ++j) {
            if (SUCCEEDED(IDXGIOutput_GetDesc(out, &od)) && od.Monitor == mon) break;
            IDXGIOutput_Release(out); out = NULL;
        }
        if (!out) { IDXGIAdapter1_Release(ad); ad = NULL; }
    }
    if (!out) goto done;
    if (FAILED(D3D11CreateDevice((IDXGIAdapter *)ad, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx))) goto done;
    g_mon = mon; g_out = out; g_dev = dev; g_ctx = ctx;
have_device:
    if (SUCCEEDED(IDXGIOutput_QueryInterface(out, &IID_IDXGIOutput5, (void **)&o5))) {
        DXGI_FORMAT f = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (FAILED(IDXGIOutput5_DuplicateOutput1(o5, (IUnknown *)dev, 0, 1, &f, &dup))) dup = NULL;
    }
    if (!dup && SUCCEEDED(IDXGIOutput_QueryInterface(out, &IID_IDXGIOutput1, (void **)&o1)))
        if (FAILED(IDXGIOutput1_DuplicateOutput(o1, (IUnknown *)dev, &dup))) dup = NULL;
    if (!dup) goto done;
    DXGI_OUTDUPL_FRAME_INFO fi;
    for (int tries = 0; tries < 4; ++tries) {   /* the first frame of a new duplication may be empty */
        if (FAILED(IDXGIOutputDuplication_AcquireNextFrame(dup, 100, &fi, &res))) goto done;
        acquired = 1;
        if (fi.LastPresentTime.QuadPart) break;
        IDXGIResource_Release(res); res = NULL; IDXGIOutputDuplication_ReleaseFrame(dup); acquired = 0;
    }
    if (!res || FAILED(IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&tex))) goto done;
    D3D11_TEXTURE2D_DESC td; ID3D11Texture2D_GetDesc(tex, &td);
    if (td.Format != DXGI_FORMAT_B8G8R8A8_UNORM && td.Format != DXGI_FORMAT_B8G8R8X8_UNORM && td.Format != DXGI_FORMAT_R8G8B8A8_UNORM) goto done;
    int rgba = td.Format == DXGI_FORMAT_R8G8B8A8_UNORM;
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0; td.MipLevels = 1; td.ArraySize = 1;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &stg))) goto done;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stg, (ID3D11Resource *)tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) goto done;
    /* window rectangle in the output's pixels (rotation ignored) */
    int x0 = scr->left - od.DesktopCoordinates.left, y0 = scr->top - od.DesktopCoordinates.top, ww = scr->right - scr->left, wh = scr->bottom - scr->top;
    if (x0 < 0) { ww += x0; x0 = 0; } if (y0 < 0) { wh += y0; y0 = 0; }
    if (x0 + ww > (int)td.Width) ww = (int)td.Width - x0; if (y0 + wh > (int)td.Height) wh = (int)td.Height - y0;
    if (ww >= META_THUMB_W && wh >= META_THUMB_H) {
        uint64_t sum = 0;
        for (int ty = 0; ty < META_THUMB_H; ++ty) {
            int sy0 = y0 + ty * wh / META_THUMB_H, sy1 = y0 + (ty + 1) * wh / META_THUMB_H;
            uint8_t *dst = bgr + (size_t)(META_THUMB_H - 1 - ty) * META_THUMB_W * 3;   /* BMP rows bottom-up */
            for (int tx = 0; tx < META_THUMB_W; ++tx) {
                int sx0 = x0 + tx * ww / META_THUMB_W, sx1 = x0 + (tx + 1) * ww / META_THUMB_W;
                uint32_t c0 = 0, c1 = 0, c2 = 0, n = 0;
                for (int y = sy0; y < sy1; y += 2) {
                    const uint8_t *row = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
                    for (int x = sx0; x < sx1; x += 2) { const uint8_t *px = row + x * 4; c0 += px[0]; c1 += px[1]; c2 += px[2]; ++n; }
                }
                if (!n) n = 1;
                uint8_t b = (uint8_t)(c0 / n), g = (uint8_t)(c1 / n), r = (uint8_t)(c2 / n);
                dst[tx * 3 + 0] = rgba ? r : b; dst[tx * 3 + 1] = g; dst[tx * 3 + 2] = rgba ? b : r;
                sum += b + g + r;
            }
        }
        ok = sum > 0;
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stg, 0);
done:
    if (stg) ID3D11Texture2D_Release(stg);
    if (tex) ID3D11Texture2D_Release(tex);
    if (res) IDXGIResource_Release(res);
    if (acquired) IDXGIOutputDuplication_ReleaseFrame(dup);
    if (dup) IDXGIOutputDuplication_Release(dup);
    if (ctx && ctx != g_ctx) ID3D11DeviceContext_Release(ctx);
    if (dev && dev != g_dev) ID3D11Device_Release(dev);
    if (o5) IDXGIOutput5_Release(o5);
    if (o1) IDXGIOutput1_Release(o1);
    if (out && out != g_out) IDXGIOutput_Release(out);
    if (ad) IDXGIAdapter1_Release(ad);
    if (fac) IDXGIFactory1_Release(fac);
    return ok;
}
int meta_capture(uint8_t *bgr) {
    HWND w = GetForegroundWindow(); DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    RECT r;
    if (!w || pid != GetCurrentProcessId() || IsIconic(w) || !GetClientRect(w, &r) || r.right < 320 || r.bottom < 180) return 0;
    POINT o = {0, 0}; ClientToScreen(w, &o);
    RECT scr = {o.x, o.y, o.x + r.right, o.y + r.bottom};
    return dup_capture(w, &scr, bgr);
}

/* ---- slotNN\info.ini, slotNN\thumb.bmp ---- */
static void put(const char *ini, const char *k, const char *fmt, ...) {
    char v[256]; va_list ap; va_start(ap, fmt); _vsnprintf_s(v, sizeof v, _TRUNCATE, fmt, ap); va_end(ap);
    WritePrivateProfileStringA("slot", k, v, ini);
}
int meta_write(const char *slot_dir, const SlotMeta *m, const char *name, const uint8_t *thumb) {
    char ini[MAX_PATH], bmp[MAX_PATH];
    _snprintf_s(ini, sizeof ini, _TRUNCATE, "%s\\info.ini", slot_dir);
    _snprintf_s(bmp, sizeof bmp, _TRUNCATE, "%s\\thumb.bmp", slot_dir);
    if (GetFileAttributesA(slot_dir) == INVALID_FILE_ATTRIBUTES) return 0;
    put(ini, "name", "%s", name ? name : ""); put(ini, "hunter", "%s", m->hunter); put(ini, "location", "%s", m->location); put(ini, "map", "%s", m->map);
    put(ini, "created", "%s", m->created); put(ini, "play_time_ms", "%llu", m->play_time_ms);
    put(ini, "level", "%d", m->level); put(ini, "insight", "%d", m->insight); put(ini, "hp", "%d", m->hp); put(ini, "hp_max", "%d", m->hp_max);
    put(ini, "echoes", "%u", m->echoes); put(ini, "same_run_only", "%d", m->same_run_only); put(ini, "pos", "%.3f %.3f %.3f", m->pos[0], m->pos[1], m->pos[2]);
    WritePrivateProfileStringA(NULL, NULL, NULL, ini);   /* flush the profile cache */
    if (thumb) {
        BITMAPFILEHEADER fh; BITMAPINFOHEADER ih; memset(&fh, 0, sizeof fh); memset(&ih, 0, sizeof ih);
        fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + META_THUMB_BYTES;
        ih.biSize = sizeof ih; ih.biWidth = META_THUMB_W; ih.biHeight = META_THUMB_H; ih.biPlanes = 1; ih.biBitCount = 24; ih.biCompression = BI_RGB; ih.biSizeImage = META_THUMB_BYTES;
        HANDLE f = CreateFileA(bmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) { DWORD wr; WriteFile(f, &fh, sizeof fh, &wr, NULL); WriteFile(f, &ih, sizeof ih, &wr, NULL); WriteFile(f, thumb, META_THUMB_BYTES, &wr, NULL); CloseHandle(f); }
    } else DeleteFileA(bmp);
    return 1;
}
int meta_read(const char *slot_dir, SlotMeta *m, char *name, size_t nn, char *thumb, size_t tn) {
    char ini[MAX_PATH], v[256];
    memset(m, 0, sizeof *m);
    _snprintf_s(ini, sizeof ini, _TRUNCATE, "%s\\info.ini", slot_dir);
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) { if (name) name[0] = 0; if (thumb) thumb[0] = 0; return 0; }
    if (name) GetPrivateProfileStringA("slot", "name", "", name, (DWORD)nn, ini);
    GetPrivateProfileStringA("slot", "location", "", m->location, sizeof m->location, ini);
    GetPrivateProfileStringA("slot", "hunter", "", m->hunter, sizeof m->hunter, ini);
    GetPrivateProfileStringA("slot", "map", "", m->map, sizeof m->map, ini);
    GetPrivateProfileStringA("slot", "created", "", m->created, sizeof m->created, ini);
    GetPrivateProfileStringA("slot", "play_time_ms", "0", v, sizeof v, ini); m->play_time_ms = _strtoui64(v, NULL, 10);
    m->level = GetPrivateProfileIntA("slot", "level", 0, ini); m->insight = GetPrivateProfileIntA("slot", "insight", 0, ini);
    m->hp = GetPrivateProfileIntA("slot", "hp", 0, ini); m->hp_max = GetPrivateProfileIntA("slot", "hp_max", 0, ini);
    m->same_run_only = GetPrivateProfileIntA("slot", "same_run_only", 0, ini);
    GetPrivateProfileStringA("slot", "echoes", "0", v, sizeof v, ini); m->echoes = (uint32_t)strtoul(v, NULL, 10);
    GetPrivateProfileStringA("slot", "pos", "0 0 0", v, sizeof v, ini); sscanf_s(v, "%f %f %f", &m->pos[0], &m->pos[1], &m->pos[2]);
    if (thumb) {
        _snprintf_s(thumb, tn, _TRUNCATE, "%s\\thumb.bmp", slot_dir);
        if (GetFileAttributesA(thumb) == INVALID_FILE_ATTRIBUTES) thumb[0] = 0;
    }
    return 1;
}
int meta_rename(const char *slot_dir, const char *name) {
    char ini[MAX_PATH]; _snprintf_s(ini, sizeof ini, _TRUNCATE, "%s\\info.ini", slot_dir);
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) return 0;
    WritePrivateProfileStringA("slot", "name", name, ini);
    WritePrivateProfileStringA(NULL, NULL, NULL, ini);
    return 1;
}
