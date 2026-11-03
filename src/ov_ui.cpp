/* ov_ui.cpp: the overlay panel (design/DESIGN_SPEC.md, mockup Main.dc.html / Toasts.dc.html) and the overlay thread.
 *
 * Everything is laid out in the mockup's 1600x900 frame and scaled by window height / 900. The panel is drawn
 * by hand on ImGui's draw lists (fonts with letter spacing, toggles, key caps, gradients); ImGui gives the
 * clicks (invisible buttons), text entry and scrolling. ui_build runs on the port's present thread inside
 * its queue lock: it only reads a snapshot (the model) that the overlay thread refreshes and never touches
 * the disk except for small ini writes after a click. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "ov.h"
#include "ov_int.h"
extern "C" {
#include "bbs_api.h"
#include "meta.h"
#include "keys.h"
#include "practice.h"
}
#pragma comment(lib, "shell32.lib")

/* ---- one place for the version and the links (About tab) ---- */
#define OV_VERSION "0.1.2"
static const char *const k_link_url[3] = {"https://github.com/billymooreq", "https://www.speedrun.com/users/billymoore", "https://www.twitch.tv/biilymoore"};
static const char *const k_link_label[3] = {"GITHUB", "SPEEDRUN.COM", "TWITCH"};

/* ---- colours (DESIGN_SPEC) ---- */
#define RGBA(r, g, b, a) IM_COL32(r, g, b, (int)((a) * 255.0f + 0.5f))
static const ImU32 C_TEXT = IM_COL32(0xe2, 0xe6, 0xec, 255), C_BRIGHT = IM_COL32(0xee, 0xf1, 0xf5, 255), C_MUTED = IM_COL32(0x9a, 0xa6, 0xb6, 255),
                   C_SOFT = IM_COL32(0xc9, 0xd1, 0xdc, 255), C_GOLD = IM_COL32(0xd9, 0xc4, 0x8f, 255), C_ACCENT = IM_COL32(0x4a, 0x6d, 0x93, 255),
                   C_ONACC = IM_COL32(0xf3, 0xf5, 0xf8, 255), C_ERR = IM_COL32(0xd9, 0x78, 0x6f, 255), C_QUIET = IM_COL32(0xcd, 0xd5, 0xdf, 255),
                   C_EMPTY = IM_COL32(0x7f, 0x8b, 0x9b, 255), C_LINK = IM_COL32(0xc9, 0xd6, 0xe6, 255);
static ImU32 hair(float a) { return RGBA(170, 190, 215, a); }

/* ---- settings and state ---- */
enum { K_TOGGLE, K_SAVE, K_LOAD, K_NEXT, K_PREV, K_POS_SAVE, K_POS_LOAD, K_IMMORTAL, K_STAMINA, K_ITEMS, K_STEALTH, K_SILENT, K_COUNT };
static const char *const k_key_label[K_COUNT] = {"Open / close menu", "Save state", "Load state", "Next slot", "Previous slot", "Save position", "Load position",
                                                 "Immortal", "Infinite stamina", "Infinite resources", "Stealth", "Silent"};
static const char *const k_key_ini[K_COUNT] = {"toggle", "", "", "", "", "save_position", "load_position", "immortal", "stamina", "consumables", "stealth", "silent"};
static const char *const k_key_def[K_COUNT] = {"Home", "", "", "", "", "none", "none", "none", "none", "none", "none", "none"};
static HotKey g_keys[K_COUNT];
static char g_ini[MAX_PATH], g_dir[MAX_PATH], g_why[256], g_points_ini[MAX_PATH];
static int g_supported;
static struct { int notify, pause, opacity, size; float offx, offy; } S = {1, 1, 85, 100, 0, 0};   /* offx/offy: panel offset from the centre, in 900p units */
static const float PH = 730, CT = 144, CB = 706;   /* panel height, content top and bottom (mockup units) */
static int g_apt = -1;   /* active teleport point (the position hotkeys use it) */
static volatile LONG g_open, g_typing;
static double g_block_until;   /* hotkeys stay off a moment after a key was assigned (its release) */
static int g_tab, g_sel = -1, g_capture = -1, g_modal;   /* modal: 0 none, 1 rename slot, 2 delete slot, 3 rename point, 4 delete point */
static int g_modal_arg; static char g_edit[64];
static char g_echo_buf[32], g_ins_buf[16]; static int g_echo_dirty, g_ins_dirty;
static SRWLOCK g_lock = SRWLOCK_INIT;   /* model, toast, points */

struct Toast { int kind; int slot; char title[32], detail[200]; double t; };   /* kind: 0 saved/loaded (underline), 1 active, 2 error, 3 info */
static Toast g_toast; static char g_msg[220]; static double g_msg_t; static int g_msg_err;

struct Point { char name[64]; uint32_t map; float pos[3], yaw; };
static Point g_pts[64]; static int g_npts;

static struct Model {
    BbsSlotInfo slot[BBS_SLOTS]; BbsStatus st; BbsOptions opt;
    PracPlayer pl; int flags[PR_COUNT];
    uint64_t used_bytes; uint64_t thumb_stamp[BBS_SLOTS];
    char slots_dir[MAX_PATH];
} M;

static double now_s(void) { static LARGE_INTEGER f; LARGE_INTEGER t; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart / f.QuadPart; }
static const char *roman(int n) { static const char *r[] = {"", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X"}; return n >= 1 && n <= 10 ? r[n] : "?"; }
static void group3(char *out, size_t n, uint64_t v) {   /* 12 480 */
    char t[32]; _snprintf_s(t, sizeof t, _TRUNCATE, "%llu", v);
    int len = (int)strlen(t), o = 0;
    for (int i = 0; i < len && o + 4 < (int)n; ++i) { if (i && (len - i) % 3 == 0) { out[o++] = ' '; } out[o++] = t[i]; }
    out[o] = 0;
}
static void when(char *out, size_t n, const char *created) {   /* "2026-11-02 12:34:56" -> "02 Nov · 12:34" */
    static const char *mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int y, mo, d, h, mi;
    if (sscanf_s(created, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) == 5 && mo >= 1 && mo <= 12) _snprintf_s(out, n, _TRUNCATE, "%02d %s \xc2\xb7 %02d:%02d", d, mon[mo - 1], h, mi);
    else _snprintf_s(out, n, _TRUNCATE, "%s", created);
}

/* ---- refusals in plain words ---- */
static void friendly(const char *raw, char *out, size_t n) {
    static const struct { const char *has, *say; } map[] = {
        {"no player", "Loading screen in progress \xe2\x80\x94 try again in a moment"},
        {"player is dead", "The hunter is dead \xe2\x80\x94 wait until you are back on your feet"},
        {"no consistent point", "The game is busy \xe2\x80\x94 try again in a moment"},
        {"is empty", "This slot is empty"},
        {"disk space", "Not enough free disk space for another slot"},
        {"disk is full", "The slot could not be written: the disk is full"},
        {"damaged", "This slot is damaged and cannot be loaded"},
        {"another bb-probe.exe", "This slot was made with another version of the game"},
        {"only in the game run", "Saved during a loading screen: it loads only until the game is restarted"},
        {"cross-process loading is off", "Loading slots from another session is turned off"},
        {"handle virtualization", "The practice tool was not loaded at game start \xe2\x80\x94 restart the game"},
        {"canonical stack", "The practice tool was not loaded at game start \xe2\x80\x94 restart the game"},
        {"older DLL", "This slot was made by an older version \xe2\x80\x94 save it again"},
        {"slot format", "This slot was made by an older version \xe2\x80\x94 save it again"},
        {"different thread set", "The area is still loading \xe2\x80\x94 try again in a moment"},
        {"different location", "Cannot load here yet \xe2\x80\x94 try again when the area has loaded"},
        {"layout differs", "Cannot load here yet \xe2\x80\x94 try again when the area has loaded"},
        {"cannot read the slot", "The slot could not be read from disk"},
        {"could not be written", "The slot could not be written to disk"},
        {"store failed", "The slot could not be written to disk"},
        {"too many memory regions", "The game is busy \xe2\x80\x94 try again in a moment"},
    };
    const char *r = strstr(raw, ": "); r = r ? r + 2 : raw;
    for (auto &m : map) if (strstr(raw, m.has)) { strncpy_s(out, n, m.say, _TRUNCATE); return; }
    _snprintf_s(out, n, _TRUNCATE, "%s", r);
    if (out[0] >= 'a' && out[0] <= 'z') out[0] -= 32;
}
static void show_msg(int err, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    AcquireSRWLockExclusive(&g_lock);
    _vsnprintf_s(g_msg, sizeof g_msg, _TRUNCATE, fmt, ap); g_msg_t = now_s(); g_msg_err = err;
    ReleaseSRWLockExclusive(&g_lock);
    va_end(ap);
}
static void toast(int kind, int slot, const char *title, const char *detail) {
    AcquireSRWLockExclusive(&g_lock);
    g_toast.kind = kind; g_toast.slot = slot; strncpy_s(g_toast.title, sizeof g_toast.title, title, _TRUNCATE);
    strncpy_s(g_toast.detail, sizeof g_toast.detail, detail, _TRUNCATE); g_toast.t = now_s();
    ReleaseSRWLockExclusive(&g_lock);
}

/* ---- ini ---- */
static void ini_put(const char *sec, const char *key, const char *v) { WritePrivateProfileStringA(sec, key, v, g_ini); }
static void ini_puti(const char *sec, const char *key, int v) { char b[16]; _snprintf_s(b, sizeof b, _TRUNCATE, "%d", v); ini_put(sec, key, b); }
static void load_settings(void) {
    char v[64], key[48];
    for (int k = 0; k < K_COUNT; ++k) {
        if (k >= K_SAVE && k <= K_PREV) { hk_parse(core_key_text(k - K_SAVE), &g_keys[k]); continue; }
        if (k == K_TOGGLE) GetPrivateProfileStringA("overlay", "toggle_key", k_key_def[k], v, sizeof v, g_ini);
        else { _snprintf_s(key, sizeof key, _TRUNCATE, "%s_key", k_key_ini[k]); GetPrivateProfileStringA("practice", key, k_key_def[k], v, sizeof v, g_ini); }
        if (!hk_parse(v, &g_keys[k])) hk_parse(k_key_def[k], &g_keys[k]);
    }
    S.size = GetPrivateProfileIntA("overlay", "size", 100, g_ini);
    if (S.size < 60) S.size = 60; if (S.size > 150) S.size = 150;
    S.offx = (float)(int)GetPrivateProfileIntA("overlay", "pos_x", 0, g_ini); S.offy = (float)(int)GetPrivateProfileIntA("overlay", "pos_y", 0, g_ini);
    S.notify = GetPrivateProfileIntA("overlay", "notifications", 1, g_ini);
    S.pause = GetPrivateProfileIntA("overlay", "pause_game", 1, g_ini);
    S.opacity = GetPrivateProfileIntA("overlay", "opacity", 85, g_ini);
    if (S.opacity < 50) S.opacity = 50; if (S.opacity > 100) S.opacity = 100;
}
static void set_key(int k, const char *text) {
    if (k >= K_SAVE && k <= K_PREV) { core_set_key(k - K_SAVE, text); hk_parse(core_key_text(k - K_SAVE), &g_keys[k]); return; }
    hk_parse(text, &g_keys[k]);
    if (k == K_TOGGLE) ini_put("overlay", "toggle_key", g_keys[k].text);
    else { char key[48]; _snprintf_s(key, sizeof key, _TRUNCATE, "%s_key", k_key_ini[k]); ini_put("practice", key, g_keys[k].text); }
}
static void points_save(void) {
    DeleteFileA(g_points_ini);
    for (int i = 0; i < g_npts; ++i) {
        char sec[16], b[64]; _snprintf_s(sec, sizeof sec, _TRUNCATE, "point%d", i + 1);
        WritePrivateProfileStringA(sec, "name", g_pts[i].name, g_points_ini);
        _snprintf_s(b, sizeof b, _TRUNCATE, "0x%08x", g_pts[i].map); WritePrivateProfileStringA(sec, "map", b, g_points_ini);
        _snprintf_s(b, sizeof b, _TRUNCATE, "%.4f %.4f %.4f %.5f", g_pts[i].pos[0], g_pts[i].pos[1], g_pts[i].pos[2], g_pts[i].yaw); WritePrivateProfileStringA(sec, "pos", b, g_points_ini);
    }
    char b[16]; _snprintf_s(b, sizeof b, _TRUNCATE, "%d", g_npts); WritePrivateProfileStringA("points", "count", b, g_points_ini);
    _snprintf_s(b, sizeof b, _TRUNCATE, "%d", g_apt + 1); WritePrivateProfileStringA("points", "active", b, g_points_ini);
}
static void points_load(void) {
    g_npts = 0;
    int n = GetPrivateProfileIntA("points", "count", 0, g_points_ini);
    for (int i = 0; i < n && g_npts < 64; ++i) {
        char sec[16], b[96]; _snprintf_s(sec, sizeof sec, _TRUNCATE, "point%d", i + 1);
        Point p = {};
        GetPrivateProfileStringA(sec, "name", "", p.name, sizeof p.name, g_points_ini);
        GetPrivateProfileStringA(sec, "map", "0", b, sizeof b, g_points_ini); p.map = (uint32_t)strtoul(b, NULL, 0);
        GetPrivateProfileStringA(sec, "pos", "", b, sizeof b, g_points_ini);
        if (sscanf_s(b, "%f %f %f %f", &p.pos[0], &p.pos[1], &p.pos[2], &p.yaw) == 4) g_pts[g_npts++] = p;
    }
    g_apt = GetPrivateProfileIntA("points", "active", 0, g_points_ini) - 1;
    if (g_apt >= g_npts) g_apt = g_npts - 1;
}

/* ---- fonts (resources 101..104, see overlay.rc) ---- */
static ImFont *F_FELL, *F_CG, *F_CGM, *F_CGI;
static const float K_FELL = 1.2686f, K_CG = 1.211f;   /* (ascent - descent) / em: ImGui sizes by line height, CSS by em */
static float g_s = 1;   /* window height / 900 */
static HMODULE self_module(void) { HMODULE m = NULL; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&self_module, &m); return m; }
static ImFont *font_res(ImGuiIO &io, int id, const char *name) {
    HMODULE m = self_module();
    HRSRC r = FindResourceA(m, MAKEINTRESOURCEA(id), (LPCSTR)RT_RCDATA);
    HGLOBAL g = r ? LoadResource(m, r) : NULL;
    void *p = g ? LockResource(g) : NULL;
    if (!p) { ov_log("overlay: font %s missing from the DLL", name); return io.Fonts->AddFontDefault(); }
    ImFontConfig fc; fc.FontDataOwnedByAtlas = false; fc.OversampleH = 2; fc.OversampleV = 1;
    strncpy_s(fc.Name, sizeof fc.Name, name, _TRUNCATE);
    return io.Fonts->AddFontFromMemoryTTF(p, (int)SizeofResource(m, r), 16.0f, &fc);
}
void ui_setup_fonts(ImGuiIO &io) {
    F_FELL = font_res(io, 101, "IM Fell English SC");
    F_CG = F_CGM = font_res(io, 103, "Cormorant Garamond Medium");   /* body text in Medium: Regular is too thin in game */
    F_CGI = font_res(io, 104, "Cormorant Garamond Italic");
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowPadding = ImVec2(0, 0); st.WindowBorderSize = 0; st.ScrollbarSize = 6; st.ScrollbarRounding = 0; st.FrameRounding = 0;
    st.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0); st.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(170 / 255.f, 190 / 255.f, 215 / 255.f, 0.25f);
    st.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(170 / 255.f, 190 / 255.f, 215 / 255.f, 0.4f); st.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(170 / 255.f, 190 / 255.f, 215 / 255.f, 0.5f);
    st.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0x4a / 255.f, 0x6d / 255.f, 0x93 / 255.f, 0.6f);
    if (!g_supported) io.MouseDrawCursor = true;
}

/* ---- drawing helpers (mockup units * g_s) ---- */
static ImVec2 g_o;   /* panel origin */
static ImDrawList *DL(void) { return ImGui::GetWindowDrawList(); }
static float fsize(ImFont *f, float css) {
    if (f != F_FELL) css = css < 16 ? css + 1.5f : css + 1.0f;   /* readability: the mockup's sizes read small in game */
    return css * (f == F_FELL ? K_FELL : K_CG) * g_s;
}
static ImVec2 P(float x, float y) { return ImVec2(g_o.x + x * g_s, g_o.y + y * g_s); }
static float text_w(ImFont *f, float css, const char *t, float track) {
    float sz = fsize(f, css), w = 0, tr = track * css * g_s; ImFontBaked *b = f->GetFontBaked(sz);
    for (const char *p = t; *p;) { unsigned c; int n = ImTextCharFromUtf8(&c, p, NULL); if (!n) break; p += n; w += b->GetCharAdvance((ImWchar)c) + tr; }
    return w - (t[0] ? tr : 0);
}
/* top-left at p (screen px); returns the width */
static float text(ImVec2 p, ImFont *f, float css, ImU32 col, const char *t, float track = 0, float bold = 0) {
    float sz = fsize(f, css);
    if (track == 0) {
        DL()->AddText(f, sz, p, col, t);
        if (bold > 0) DL()->AddText(f, sz, ImVec2(p.x + bold * g_s, p.y), col, t);
        return f->CalcTextSizeA(sz, FLT_MAX, 0, t).x;
    }
    float x = p.x, tr = track * css * g_s; ImFontBaked *b = f->GetFontBaked(sz);
    for (const char *q = t; *q;) {
        unsigned c; int n = ImTextCharFromUtf8(&c, q, NULL); if (!n) break;
        DL()->AddText(f, sz, ImVec2(x, p.y), col, q, q + n);
        if (bold > 0) DL()->AddText(f, sz, ImVec2(x + bold * g_s, p.y), col, q, q + n);
        x += b->GetCharAdvance((ImWchar)c) + tr; q += n;
    }
    return x - p.x - tr;
}
static float lh(ImFont *f, float css) { return fsize(f, css); }   /* line height = ImGui size */
/* text vertically centred on cy */
static float textc(float x, float cy, ImFont *f, float css, ImU32 col, const char *t, float track = 0) { return text(ImVec2(x, cy - lh(f, css) / 2), f, css, col, t, track); }
static void rect(ImVec2 a, ImVec2 b, ImU32 fill, ImU32 border = 0, float rounding = 0) {
    if (fill & IM_COL32_A_MASK) DL()->AddRectFilled(a, b, fill, rounding);
    if (border & IM_COL32_A_MASK) DL()->AddRect(a, b, border, rounding, ImDrawFlags_None, 1.0f);
}
static void hline(float x0, float x1, float y, ImU32 c) { DL()->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + ImMax(1.0f, g_s * 0.9f)), c); }
static ImVec2 operator+(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
/* soft shadow: rings from the rect outwards, alpha a -> 0 */
static void shadow(ImVec2 a, ImVec2 b, float spread, float alpha) {
    ImDrawList *dl = DL(); const int R = 6;
    for (int i = 0; i < R; ++i) {
        float e0 = spread * i / R, e1 = spread * (i + 1) / R;
        float a0 = alpha * (1 - (float)i / R) * (1 - (float)i / R), a1 = alpha * (1 - (float)(i + 1) / R) * (1 - (float)(i + 1) / R);
        ImVec2 o0[4] = {{a.x - e0, a.y - e0}, {b.x + e0, a.y - e0}, {b.x + e0, b.y + e0}, {a.x - e0, b.y + e0}};
        ImVec2 o1[4] = {{a.x - e1, a.y - e1}, {b.x + e1, a.y - e1}, {b.x + e1, b.y + e1}, {a.x - e1, b.y + e1}};
        ImU32 c0 = IM_COL32(0, 0, 0, (int)(a0 * 255)), c1 = IM_COL32(0, 0, 0, (int)(a1 * 255));
        ImVec2 uv = dl->_Data->TexUvWhitePixel;
        dl->PrimReserve(24, 16);
        for (int k = 0; k < 4; ++k) {
            int k2 = (k + 1) & 3;
            ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
            dl->PrimWriteVtx(o0[k], uv, c0); dl->PrimWriteVtx(o0[k2], uv, c0); dl->PrimWriteVtx(o1[k2], uv, c1); dl->PrimWriteVtx(o1[k], uv, c1);
            dl->PrimWriteIdx(base); dl->PrimWriteIdx(base + 1); dl->PrimWriteIdx(base + 2); dl->PrimWriteIdx(base); dl->PrimWriteIdx(base + 2); dl->PrimWriteIdx(base + 3);
        }
        dl->PrimUnreserve(0, 0);
    }
    DL()->AddRectFilled(a, b, IM_COL32(0, 0, 0, (int)(alpha * 255)));
}
/* full screen radial vignette: alpha a0 at the centre, a1 at the corners */
static void vignette(ImDrawList *dl, float w, float h, float a0, float a1) {
    const int GX = 16, GY = 10; ImVec2 uv = dl->_Data->TexUvWhitePixel;
    dl->PrimReserve(GX * GY * 6, (GX + 1) * (GY + 1));
    ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
    for (int y = 0; y <= GY; ++y) for (int x = 0; x <= GX; ++x) {
        float u = x / (float)GX * 2 - 1, v = y / (float)GY * 2 - 1, r = sqrtf((u * u + v * v) / 2);
        dl->PrimWriteVtx(ImVec2(w * x / GX, h * y / GY), uv, IM_COL32(4, 7, 12, (int)((a0 + (a1 - a0) * r) * 255)));
    }
    for (int y = 0; y < GY; ++y) for (int x = 0; x < GX; ++x) {
        ImDrawIdx i = (ImDrawIdx)(base + y * (GX + 1) + x);
        dl->PrimWriteIdx(i); dl->PrimWriteIdx(i + 1); dl->PrimWriteIdx(i + GX + 2); dl->PrimWriteIdx(i); dl->PrimWriteIdx(i + GX + 2); dl->PrimWriteIdx(i + GX + 1);
    }
}
static void glow(ImVec2 c, float r, ImU32 rgb, float a) {   /* radial gradient disc, alpha a -> 0 */
    ImDrawList *dl = DL(); const int N = 48; ImVec2 uv = dl->_Data->TexUvWhitePixel;
    dl->PrimReserve(N * 3, N + 1);
    ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(c, uv, (rgb & 0xffffff) | ((ImU32)(a * 255) << 24));
    for (int i = 0; i < N; ++i) dl->PrimWriteVtx(ImVec2(c.x + cosf(i * 6.2831853f / N) * r, c.y + sinf(i * 6.2831853f / N) * r), uv, rgb & 0xffffff);
    for (int i = 0; i < N; ++i) { dl->PrimWriteIdx(base); dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i)); dl->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % N)); }
}

/* ---- widgets ---- */
static int g_id;
static bool hit(ImVec2 a, ImVec2 b, bool *hov = NULL, bool any_modal = false) {
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(++g_id);
    bool c = ImGui::InvisibleButton("b", ImVec2(ImMax(1.0f, b.x - a.x), ImMax(1.0f, b.y - a.y)));
    bool h = ImGui::IsItemHovered();
    ImGui::PopID();
    if (g_modal && !any_modal) { c = false; h = false; }
    if (hov) *hov = h;
    return c;
}
enum { B_PRIMARY, B_OUTLINE, B_QUIET, B_LINK };
/* button at panel coords (x, y), height h, horizontal padding pad; returns clicked; *w gets its width */
static bool button(float x, float y, float h, float pad, const char *label, int kind, float css = 14, float track = 0.18f, float *w_out = NULL, bool modal = false, float fixed_w = 0) {
    float tw = text_w(F_FELL, css, label, track), w = fixed_w > 0 ? fixed_w : tw / g_s + 2 * pad;
    ImVec2 a = P(x, y), b = P(x + w, y + h);
    bool hov; bool c = hit(a, b, &hov, modal);
    ImU32 fill = 0, border = 0, col = C_BRIGHT;
    if (kind == B_PRIMARY) { fill = hov ? IM_COL32(0x58, 0x7d, 0xa6, 255) : C_ACCENT; border = fill; col = C_ONACC; }
    else if (kind == B_OUTLINE) { fill = RGBA(255, 255, 255, hov ? 0.07f : 0.02f); border = hair(hov ? 0.7f : 0.5f); }
    else if (kind == B_QUIET) { fill = hov ? RGBA(255, 255, 255, 0.04f) : 0; border = hair(hov ? 0.45f : 0.25f); col = C_QUIET; }
    else { fill = hov ? RGBA(255, 255, 255, 0.04f) : 0; border = hair(0.45f); col = hov ? C_ONACC : C_LINK; }
    rect(a, b, fill, border);
    text(ImVec2((a.x + b.x - tw) / 2, (a.y + b.y) / 2 - lh(F_FELL, css) / 2), F_FELL, css, col, label, track);
    if (w_out) *w_out = w;
    return c;
}
static bool toggle(float x, float y, bool on, bool modal = false) {   /* 46x26 at panel coords */
    ImVec2 a = P(x, y), b = P(x + 46, y + 26);
    bool hov; bool c = hit(a, b, &hov, modal);
    float r = 13 * g_s;
    if (on) DL()->AddRectFilled(a, b, C_ACCENT, r);
    else { DL()->AddRectFilled(a, b, RGBA(255, 255, 255, 0.06f), r); DL()->AddRect(a, b, hair(hov ? 0.55f : 0.35f), r, ImDrawFlags_None, 1.0f); }
    ImVec2 k = P(x + 3 + 9 + (on ? 20 : 0), y + 13);
    DL()->AddCircleFilled(k, 9 * g_s, on ? C_ONACC : C_MUTED, 24);
    return c;
}
static float keycap(float x, float cy, const char *t, float css, float padx, float pady, ImU32 border, ImU32 col, float minw = 0) {
    float tw = text_w(F_CGM, css, t, 0), w = ImMax(tw / g_s + 2 * padx, minw), h = lh(F_CGM, css) / g_s + 2 * pady;
    ImVec2 a = P(x, cy - h / 2), b = P(x + w, cy + h / 2);
    rect(a, b, 0, border, 3 * g_s);
    text(ImVec2((a.x + b.x - tw) / 2, (a.y + b.y - lh(F_CGM, css)) / 2), F_CGM, css, col, t);
    return w;
}
static const char *key_label(const HotKey &k) {   /* "SHIFT + F6", "—" */
    static char b[8][48]; static int bi; char *o = b[bi++ & 7];
    if (!k.vk) return "\xe2\x80\x94";
    _snprintf_s(o, 48, _TRUNCATE, "%s%s%s%s", k.ctrl ? "CTRL + " : "", k.alt ? "ALT + " : "", k.shift ? "SHIFT + " : "", hk_vk_name(k.vk));
    for (char *p = o; *p; ++p) *p = (char)toupper((unsigned char)*p);
    return o;
}
/* styled text field at panel coords; returns true when Enter was pressed */
static bool field(const char *id, float x, float y, float w, float h, char *buf, size_t n, float css, int flags = 0, bool modal = false) {
    ImVec2 a = P(x, y), b = P(x + w, y + h);
    rect(a, b, RGBA(0, 0, 0, 0.35f), hair(0.3f));
    if (g_modal && !modal) { text(ImVec2(a.x + 14 * g_s, (a.y + b.y - lh(F_CG, css)) / 2), F_CG, css, C_BRIGHT, buf); return false; }
    ImGui::SetCursorScreenPos(a);
    ImGui::PushFont(F_CG, fsize(F_CG, css));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14 * g_s, (h * g_s - ImGui::GetFontSize()) / 2));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0)); ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(1, 1, 1, 0.03f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(1, 1, 1, 0.04f)); ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0xee / 255.f, 0xf1 / 255.f, 0xf5 / 255.f, 1));
    ImGui::SetNextItemWidth(w * g_s);
    bool enter = ImGui::InputText(id, buf, n, flags | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor(4); ImGui::PopStyleVar(); ImGui::PopFont();
    return enter;
}
static void corner(ImVec2 o, float sx, float sy) {   /* 28x28 ornament, mirrored by sx/sy */
    auto Q = [&](float x, float y) { return ImVec2(o.x + sx * x * g_s, o.y + sy * y * g_s); };
    ImVec2 l[3] = {Q(1, 27), Q(1, 1), Q(27, 1)};
    DL()->AddPolyline(l, 3, C_GOLD, 0, 1.2f * g_s);
    DL()->AddQuadFilled(Q(7, 4), Q(10, 7), Q(7, 10), Q(4, 7), C_GOLD);
}
static void icon_error(ImVec2 c, float r) {
    DL()->AddCircle(c, r, C_ERR, 32, 1.4f * g_s);
    float k = r * 0.45f;
    DL()->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), C_ERR, 1.4f * g_s);
    DL()->AddLine(ImVec2(c.x + k, c.y - k), ImVec2(c.x - k, c.y + k), C_ERR, 1.4f * g_s);
}

/* ---- the snapshot the panel draws from ---- */
static Model V;   /* copy of M for this frame */
static bool slot_used(int i) { return V.slot[i].used != 0; }
static const char *slot_name(int i) { return slot_used(i) ? V.slot[i].name : "Empty slot"; }

/* ---- header, tabs, footer ---- */
static void resize_edges(float W, float H) {   /* drag an edge or a corner: the opposite side stays put */
    static int edges; static float l0, t0, r0, b0;
    float base = H / 900.0f, e = 7, pw = 1120 * g_s, ph = PH * g_s;
    ImVec2 o = g_o;
    const struct { float x0, y0, x1, y1; int f; } z[8] = {
        {o.x - e, o.y - e, o.x + e, o.y + e, 1 | 4}, {o.x + pw - e, o.y - e, o.x + pw + e, o.y + e, 2 | 4},
        {o.x - e, o.y + ph - e, o.x + e, o.y + ph + e, 1 | 8}, {o.x + pw - e, o.y + ph - e, o.x + pw + e, o.y + ph + e, 2 | 8},
        {o.x - e, o.y + e, o.x + e, o.y + ph - e, 1}, {o.x + pw - e, o.y + e, o.x + pw + e, o.y + ph - e, 2},
        {o.x + e, o.y - e, o.x + pw - e, o.y + e, 4}, {o.x + e, o.y + ph - e, o.x + pw - e, o.y + ph + e, 8}};
    for (int i = 0; i < 8; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(z[i].x0, z[i].y0));
        ImGui::PushID(1000 + i);
        ImGui::InvisibleButton("rz", ImVec2(z[i].x1 - z[i].x0, z[i].y1 - z[i].y0));
        ImGui::PopID();
        if (g_modal) continue;
        if (ImGui::IsItemActivated()) { edges = z[i].f; l0 = o.x; t0 = o.y; r0 = o.x + pw; b0 = o.y + ph; }
        if (ImGui::IsItemActive() && edges) {
            ImVec2 m = ImGui::GetIO().MousePos; float sx = 0, sy = 0;
            if (edges & 2) sx = (m.x - l0) / (1120 * base); if (edges & 1) sx = (r0 - m.x) / (1120 * base);
            if (edges & 8) sy = (m.y - t0) / (PH * base); if (edges & 4) sy = (b0 - m.y) / (PH * base);
            int sz = (int)(ImMax(sx, sy) * 100 + 0.5f); sz = sz < 60 ? 60 : sz > 150 ? 150 : sz;
            float gs = base * sz / 100, nw = 1120 * gs, nh = PH * gs;
            float nl = (edges & 1) ? r0 - nw : l0, nt = (edges & 4) ? b0 - nh : t0;
            S.size = sz; S.offx = (nl - (W - nw) / 2) / base; S.offy = (nt - (H - nh) / 2) / base;
        }
        if (ImGui::IsItemDeactivated()) {
            edges = 0;
            ini_puti("overlay", "size", S.size); ini_puti("overlay", "pos_x", (int)S.offx); ini_puti("overlay", "pos_y", (int)S.offy);
        }
    }
    /* grip in the bottom right corner */
    for (int k = 1; k <= 3; ++k) DL()->AddLine(P(1120 - 4 - 5 * k, PH - 4), P(1120 - 4, PH - 4 - 5 * k), hair(0.35f), 1.0f);
}
static void draw_frame(float W, float H) {
    ImDrawList *bg = DL();
    if (S.pause) vignette(bg, W, H, 0.25f, 0.75f);   /* the game runs behind: no dimming */
    ImVec2 a = P(0, 0), b = P(1120, PH);
    shadow(ImVec2(a.x, a.y + 30 * g_s), ImVec2(b.x, b.y + 30 * g_s), 90 * g_s, 0.45f);
    rect(a, b, IM_COL32(10, 16, 24, (int)(S.opacity * 2.55f + 0.5f)), hair(0.22f));
    corner(P(-1, -1), 1, 1); corner(P(1121, -1), -1, 1); corner(P(-1, PH + 1), 1, -1); corner(P(1121, PH + 1), -1, -1);
    resize_edges(W, H);
    /* header: the title; right: ABOUT and the last message (results and refusals in plain words) */
    text(P(40, 26), F_FELL, 26, C_BRIGHT, "PRACTICE TOOL", 0.18f, 0.6f);
    float cy = 26 + 17;
    float aw = text_w(F_FELL, 13, "ABOUT", 0.22f) / g_s;
    bool ah; if (hit(P(1080 - aw - 8, cy - 14), P(1088, cy + 14), &ah)) { g_tab = g_tab == 4 ? 0 : 4; g_capture = -1; }
    text(P(1080 - aw, cy - lh(F_FELL, 13) / g_s / 2), F_FELL, 13, g_tab == 4 ? C_ONACC : ah ? C_SOFT : C_MUTED, "ABOUT", 0.22f);
    if (g_tab == 4) DL()->AddRectFilled(P(1080 - aw, cy + 12), P(1080, cy + 13.5f), C_ACCENT);
    char st[240] = ""; ImU32 dot = C_GOLD;
    double age = now_s() - g_msg_t;
    if (!g_supported) { _snprintf_s(st, sizeof st, _TRUNCATE, "Not available for this version"); dot = C_ERR; }
    else if (V.st.state == BBS_SAVING) _snprintf_s(st, sizeof st, _TRUNCATE, "Saving\xe2\x80\xa6");
    else if (V.st.state == BBS_LOADING) _snprintf_s(st, sizeof st, _TRUNCATE, "Loading\xe2\x80\xa6");
    else if (g_msg[0] && age < 5) { AcquireSRWLockShared(&g_lock); strncpy_s(st, sizeof st, g_msg, _TRUNCATE); ReleaseSRWLockShared(&g_lock); dot = g_msg_err ? C_ERR : IM_COL32(0xa9, 0xc4, 0xde, 255); }
    if (st[0]) {
        float sw = text_w(F_CG, 15, st, 0) / g_s, sx = 1080 - aw - 30 - sw;
        if (sx < 330) sx = 330;
        ImGui::PushClipRect(P(sx, cy - 15), P(1080 - aw - 24, cy + 15), true);
        textc(P(sx, 0).x, P(0, cy).y, F_CG, 15, C_SOFT, st);
        ImGui::PopClipRect();
        ImVec2 dc = P(sx - 12, cy);
        glow(dc, 8 * g_s, dot, 0.5f);
        DL()->AddCircleFilled(dc, 3.5f * g_s, dot, 16);
    }
    /* drag the panel by its header (after the ABOUT button: that one wins its own clicks) */
    hit(P(0, 0), P(1120, 74));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 0) && !g_modal) {
        ImVec2 d = ImGui::GetIO().MouseDelta; float base = H / 900.0f;
        S.offx += d.x / base; S.offy += d.y / base;
    }
    if (ImGui::IsItemDeactivated()) { ini_puti("overlay", "pos_x", (int)S.offx); ini_puti("overlay", "pos_y", (int)S.offy); }
    /* tabs */
    static const char *tabs[] = {"SAVESTATES", "PRACTICE", "TELEPORT", "SETTINGS"};
    float tx = 40, ty = 74, th = 14 + lh(F_FELL, 14) / g_s + 12;
    hline(P(40, 0).x, P(1080, 0).x, P(0, ty + th).y, hair(0.18f));
    for (int i = 0; i < 4; ++i) {
        float w = text_w(F_FELL, 14, tabs[i], 0.22f) / g_s + 40;
        bool hov; if (hit(P(tx, ty), P(tx + w, ty + th), &hov)) { g_tab = i; g_capture = -1; }
        bool cur = g_tab == i;
        if (cur) DL()->AddRectFilled(P(tx, ty + th - 1), P(tx + w, ty + th + 1), C_ACCENT);
        text(P(tx + 20, ty + 14), F_FELL, 14, cur ? C_ONACC : hov ? C_SOFT : C_MUTED, tabs[i], 0.22f);
        tx += w + 6;
    }
}

/* ---- SAVESTATES ---- */
static void thumb_draw(int id, ImVec2 a, ImVec2 b) {
    int w, h; ImTextureID t = ovtex_id(id, &w, &h);
    if (t) {   /* fill the box without stretching: crop the 16:9 picture to the box's shape */
        float bw = b.x - a.x, bh = b.y - a.y, ia = h > 0 ? (float)w / h : 16.f / 9, ba = bh > 0 ? bw / bh : ia;
        ImVec2 u0(0, 0), u1(1, 1);
        if (ba > ia) { float k = ia / ba; u0.y = (1 - k) / 2; u1.y = 1 - u0.y; } else { float k = ba / ia; u0.x = (1 - k) / 2; u1.x = 1 - u0.x; }
        DL()->AddImage(ImTextureRef(t), a, b, u0, u1);
    }
    else DL()->AddRectFilledMultiColor(a, b, IM_COL32(0x22, 0x34, 0x4a, 255), IM_COL32(0x22, 0x34, 0x4a, 255), IM_COL32(0x1d, 0x24, 0x30, 255), IM_COL32(0x1d, 0x24, 0x30, 255));
}
static void dashed_rect(ImVec2 a, ImVec2 b, ImU32 c) {
    float d = 3 * g_s;
    for (float x = a.x; x < b.x; x += 2 * d) { DL()->AddLine(ImVec2(x, a.y + 0.5f), ImVec2(ImMin(x + d, b.x), a.y + 0.5f), c); DL()->AddLine(ImVec2(x, b.y - 0.5f), ImVec2(ImMin(x + d, b.x), b.y - 0.5f), c); }
    for (float y = a.y; y < b.y; y += 2 * d) { DL()->AddLine(ImVec2(a.x + 0.5f, y), ImVec2(a.x + 0.5f, ImMin(y + d, b.y)), c); DL()->AddLine(ImVec2(b.x - 0.5f, y), ImVec2(b.x - 0.5f, ImMin(y + d, b.y)), c); }
}
static void tab_savestates(void) {
    if (g_sel < 0) g_sel = (V.st.active_slot >= 1 ? V.st.active_slot : 1) - 1;
    int used = 0; for (int i = 0; i < BBS_SLOTS; ++i) used += slot_used(i);
    float y0 = CT;
    text(P(40, y0 + 2), F_FELL, 13, C_GOLD, "SLOTS", 0.22f);
    char info[96]; _snprintf_s(info, sizeof info, _TRUNCATE, "%d of 10 used \xc2\xb7 %.1f GB on disk", used, V.used_bytes / 1073741824.0);
    float iw = text_w(F_CG, 14, info, 0) / g_s;
    text(P(480 - iw, y0), F_CG, 14, C_MUTED, info);
    for (int i = 0; i < BBS_SLOTS; ++i) {
        float ry = y0 + 27 + i * 46;
        ImVec2 a = P(40, ry), b = P(480, ry + 42);
        bool hov; if (hit(a, b, &hov)) g_sel = i;
        bool sel = g_sel == i;
        rect(a, b, sel ? hair(0.09f) : hov ? hair(0.05f) : RGBA(0, 0, 0, 0.15f), sel ? hair(0.4f) : hair(0.08f));
        float cy = ry + 21;
        textc(P(52, 0).x, P(0, cy).y, F_FELL, 14, C_GOLD, roman(i + 1));
        ImVec2 ta = P(96, cy - 15), tb = P(152, cy + 15);
        if (slot_used(i)) thumb_draw(OVTEX_MINI0 + i, ta, tb); else dashed_rect(ta, tb, hair(0.3f));
        float nx = 166, right = 468;
        if (V.st.active_slot == i + 1) {   /* the slot the save / load keys use */
            float pw = text_w(F_CGM, 12, "ACTIVE", 0.12f) / g_s + 16, ph = lh(F_CGM, 12) / g_s + 4;
            rect(P(right - pw, cy - ph / 2), P(right, cy + ph / 2), C_ACCENT);
            text(P(right - pw + 8, cy - ph / 2 + 2), F_CGM, 12, C_ONACC, "ACTIVE", 0.12f);
            right -= pw + 14;
        }
        ImGui::PushClipRect(P(nx, ry), P(right, ry + 42), true);
        float l1 = lh(F_CG, 16) * 0.95f / g_s, l2 = lh(F_CG, 13) * 0.95f / g_s, top = cy - (l1 + l2) / 2;
        text(P(nx, top), slot_used(i) ? F_CG : F_CGI, 16, slot_used(i) ? C_BRIGHT : C_EMPTY, slot_name(i));
        char sub[128] = "\xe2\x80\x94";
        if (slot_used(i)) { char wd[48]; when(wd, sizeof wd, V.slot[i].created); _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s \xc2\xb7 %s", V.slot[i].location[0] ? V.slot[i].location : V.slot[i].map, wd); }
        text(P(nx, top + l1), F_CG, 13, C_MUTED, sub);
        ImGui::PopClipRect();
    }
    /* details */
    int s = g_sel; const BbsSlotInfo &si = V.slot[s]; bool used_ = slot_used(s);
    float x0 = 516, x1 = 1080, y = y0;
    ImVec2 a = P(x0, y), b = P(x1, y + 250);
    if (used_ && ovtex_id(OVTEX_SLOT0 + s, NULL, NULL)) thumb_draw(OVTEX_SLOT0 + s, a, b);
    else rect(a, b, used_ ? IM_COL32(0x15, 0x22, 0x34, 255) : RGBA(0, 0, 0, 0.3f));
    DL()->AddRectFilledMultiColor(ImVec2(a.x, b.y - 46 * g_s), b, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 190), IM_COL32(0, 0, 0, 190));
    rect(a, b, 0, hair(0.25f));
    text(P(x0 + 18, y + 250 - 14 - lh(F_CGI, 14) / g_s), F_CGI, 14, C_SOFT, used_ && ovtex_id(OVTEX_SLOT0 + s, NULL, NULL) ? "Screenshot taken at the moment of saving" : "No screenshot");
    y += 250 + 18;
    float nl = lh(F_FELL, 22) / g_s;
    float numw = text(P(x0, y + (lh(F_FELL, 22) - lh(F_FELL, 18)) / g_s * 0.75f), F_FELL, 18, C_GOLD, roman(s + 1)) / g_s;
    ImGui::PushClipRect(P(x0, y), P(x1, y + nl), true);
    text(P(x0 + numw + 14, y), F_FELL, 22, C_BRIGHT, slot_name(s), 0.06f);
    ImGui::PopClipRect();
    y += nl + 18;
    if (used_) {
        char loc[80], wd[48], lv[96], hp[48], ech[48], pt[48];
        _snprintf_s(loc, sizeof loc, _TRUNCATE, "%s", si.location[0] ? si.location : si.map);
        when(wd, sizeof wd, si.created);
        if (si.hunter[0]) _snprintf_s(lv, sizeof lv, _TRUNCATE, "%s \xc2\xb7 Lv %d", si.hunter, si.level); else _snprintf_s(lv, sizeof lv, _TRUNCATE, "Level %d", si.level);
        char h1[16], h2[16]; group3(h1, sizeof h1, (uint64_t)ImMax(0, si.hp)); group3(h2, sizeof h2, (uint64_t)ImMax(0, si.hp_max));
        _snprintf_s(hp, sizeof hp, _TRUNCATE, "%s / %s", h1, h2);
        group3(ech, sizeof ech, si.echoes);
        _snprintf_s(pt, sizeof pt, _TRUNCATE, "%llu h %02llu m", si.play_time_ms / 3600000, si.play_time_ms / 60000 % 60);
        const char *lab[6] = {"LOCATION", "DATE", "CHARACTER", "HP", "BLOOD ECHOES", "PLAYTIME"}, *val[6] = {loc, wd, lv, hp, ech, pt};
        float cw = (x1 - x0 - 48) / 3, rh = lh(F_CG, 12) / g_s + lh(F_CG, 15) / g_s;
        for (int k = 0; k < 6; ++k) {
            float cx = x0 + (k % 3) * (cw + 24), ry = y + (k / 3) * (rh + 12);
            text(P(cx, ry), F_CG, 12, C_MUTED, lab[k], 0.16f);
            ImGui::PushClipRect(P(cx, ry), P(cx + cw, ry + rh), true);
            text(P(cx, ry + lh(F_CG, 12) / g_s), F_CG, 15, C_TEXT, val[k]);
            ImGui::PopClipRect();
        }
        y += 2 * rh + 12;
        if (si.same_run_only || si.damaged) {
            text(P(x0, y + 2), F_CGI, 14, si.damaged ? C_ERR : C_MUTED, si.damaged ? "Damaged: some of its data cannot be read" : "Saved during loading: loads only until the game is restarted");
        }
    } else {
        ImGui::PushTextWrapPos(P(x1, 0).x);
        ImGui::SetCursorScreenPos(P(x0, y));
        ImGui::PushFont(F_CGI, fsize(F_CGI, 16)); ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(C_MUTED));
        ImGui::TextWrapped("This slot is empty. Save here to capture the whole world \xe2\x80\x94 enemies, bosses and all \xe2\x80\x94 exactly as it is now.");
        ImGui::PopStyleColor(); ImGui::PopFont(); ImGui::PopTextWrapPos();
        y += 2 * (lh(F_CG, 12) / g_s + lh(F_CG, 15) / g_s) + 12;
    }
    y += 18;
    float bx = x0, w;
    bool busy = V.st.state != BBS_READY || !g_supported;
    if (button(bx, y, 44, 26, "LOAD", B_PRIMARY, 14, 0.18f, &w) && !busy) {
        if (!used_) show_msg(1, "Cannot load: this slot is empty");
        else { bbs_load(s + 1); show_msg(0, "Loading slot %s\xe2\x80\xa6", roman(s + 1)); }
    }
    bx += w + 10;
    if (button(bx, y, 44, 22, "SAVE HERE", B_OUTLINE, 14, 0.18f, &w) && !busy) { bbs_save(s + 1); show_msg(0, "Saving to slot %s\xe2\x80\xa6", roman(s + 1)); }
    bx += w + 10;
    if (button(bx, y, 44, 18, "RENAME", B_QUIET, 13, 0.16f, &w) && used_) { g_modal = 1; g_modal_arg = s; strncpy_s(g_edit, sizeof g_edit, si.name, _TRUNCATE); }
    bx += w + 10;
    if (button(bx, y, 44, 18, "DELETE", B_QUIET, 13, 0.16f, &w) && used_ && !busy) { g_modal = 2; g_modal_arg = s; }
    y += 44 + 18;
    hline(P(x0, 0).x, P(x1, 0).x, P(0, y).y, hair(0.15f));
    y += 14;
    float cy = y + 20;
    if (V.st.active_slot == s + 1) textc(P(x0, 0).x, P(0, cy).y, F_CG, 15, C_SOFT, "Active slot");
    else if (button(x0, y, 40, 18, "MAKE ACTIVE", B_OUTLINE, 12, 0.16f) && g_supported) bbs_set_active(s + 1);
    const char *dl_ = "Load active slot on death";
    float dw = text_w(F_CG, 15, dl_, 0) / g_s;
    textc(P(x1 - 46 - 12 - dw, 0).x, P(0, cy).y, F_CG, 15, C_SOFT, dl_);
    if (toggle(x1 - 46, cy - 13, V.opt.autoload_on_death != 0) && g_supported) { BbsOptions o = V.opt; o.autoload_on_death = !o.autoload_on_death; bbs_set_options(&o); V.opt = o; M.opt = o; }
}

/* ---- PRACTICE ---- */
static void flag_toggle(int f) {
    if (!g_supported) return;
    int on = !prac_flag(f);
    if (!prac_set_flag(f, on)) show_msg(1, "This option is not available right now");
}
static void tab_practice(void) {
    static const char *lab[PR_COUNT] = {"Immortal", "Infinite stamina", "Infinite resources", "Stealth", "Silent"};
    static const char *desc[PR_COUNT] = {"HP never drops below 1", "Stamina is never used up", "Blood vials, bullets and items are never used up", "Enemies do not see you", "Enemies do not hear you"};
    float y = CT;
    text(P(40, y), F_FELL, 13, C_GOLD, "HUNTER", 0.22f);
    y += lh(F_FELL, 13) / g_s + 8 + 6;
    for (int i = 0; i < PR_COUNT; ++i) {
        float rh = 12 + lh(F_CG, 18) / g_s + lh(F_CG, 14) / g_s + 12, cy = y + rh / 2;
        text(P(40, y + 12), F_CG, 18, C_BRIGHT, lab[i]);
        text(P(40, y + 12 + lh(F_CG, 18) / g_s), F_CG, 14, C_MUTED, desc[i]);
        const HotKey &k = g_keys[K_IMMORTAL + i];
        float kw = text_w(F_FELL, 11, key_label(k), 0) / g_s + 16; if (kw < 34) kw = 34;
        keycap(520 - 46 - 16 - kw, cy, key_label(k), 11, 8, 2, hair(0.3f), C_MUTED, 34);
        if (toggle(520 - 46, cy - 13, V.flags[i] != 0)) { flag_toggle(i); M.flags[i] = V.flags[i] = prac_flag(i); }
        hline(P(40, 0).x, P(520, 0).x, P(0, y + rh).y, hair(0.12f));
        y += rh + 1 + 6;
    }
    /* resources */
    float x0 = 560, x1 = 1080; y = CT;
    text(P(x0, y), F_FELL, 13, C_GOLD, "RESOURCES", 0.22f);
    y += lh(F_FELL, 13) / g_s + 22;
    ImGuiID eid = ImGui::GetID("##echoes"), iid = ImGui::GetID("##insight");
    ImGuiContext &g = *GImGui;
    if (g.ActiveId != eid && !g_echo_dirty) _snprintf_s(g_echo_buf, sizeof g_echo_buf, _TRUNCATE, "%u", V.pl.echoes);
    if (g.ActiveId != iid && !g_ins_dirty) _snprintf_s(g_ins_buf, sizeof g_ins_buf, _TRUNCATE, "%d", V.pl.insight);
    char cur[64], num[32];
    text(P(x0, y), F_CG, 15, C_SOFT, "Blood Echoes");
    group3(num, sizeof num, V.pl.echoes); _snprintf_s(cur, sizeof cur, _TRUNCATE, "now %s", V.pl.ok ? num : "\xe2\x80\x94");
    text(P(x1 - text_w(F_CG, 14, cur, 0) / g_s, y + 1), F_CG, 14, C_MUTED, cur);
    y += lh(F_CG, 15) / g_s + 8;
    float w1 = text_w(F_FELL, 12, "+10 000", 0.14f) / g_s + 32, w2 = text_w(F_FELL, 12, "SET", 0.14f) / g_s + 32;
    float fw = x1 - x0 - w1 - w2 - 20;
    bool enter = field("##echoes", x0, y, fw, 44, g_echo_buf, sizeof g_echo_buf, 18, ImGuiInputTextFlags_CharsDecimal);
    if (ImGui::IsItemEdited()) g_echo_dirty = 1;
    if (button(x0 + fw + 10, y, 44, 16, "+10 000", B_OUTLINE, 12, 0.14f) && V.pl.ok) {
        uint64_t v = (uint64_t)V.pl.echoes + 10000; if (v > 999999999) v = 999999999;
        if (prac_set_echoes((uint32_t)v)) show_msg(0, "Blood Echoes: %u", (uint32_t)v); g_echo_dirty = 0;
    }
    if ((button(x0 + fw + 20 + w1, y, 44, 16, "SET", B_PRIMARY, 12, 0.14f) || enter) && V.pl.ok) {
        unsigned long long v = strtoull(g_echo_buf, NULL, 10); if (v > 999999999) v = 999999999;
        if (prac_set_echoes((uint32_t)v)) show_msg(0, "Blood Echoes: %llu", v); g_echo_dirty = 0;
    }
    y += 44 + 22;
    text(P(x0, y), F_CG, 15, C_SOFT, "Insight");
    _snprintf_s(cur, sizeof cur, _TRUNCATE, "now %d", V.pl.insight);
    text(P(x1 - text_w(F_CG, 14, V.pl.ok ? cur : "now \xe2\x80\x94", 0) / g_s, y + 1), F_CG, 14, C_MUTED, V.pl.ok ? cur : "now \xe2\x80\x94");
    y += lh(F_CG, 15) / g_s + 8;
    fw = x1 - x0 - w2 - 10;
    enter = field("##insight", x0, y, fw, 44, g_ins_buf, sizeof g_ins_buf, 18, ImGuiInputTextFlags_CharsDecimal);
    if (ImGui::IsItemEdited()) g_ins_dirty = 1;
    if ((button(x0 + fw + 10, y, 44, 16, "SET", B_PRIMARY, 12, 0.14f) || enter) && V.pl.ok) {
        int v = atoi(g_ins_buf); if (v < 0) v = 0; if (v > 999) v = 999;
        if (prac_set_insight(v)) show_msg(0, "Insight: %d", v); g_ins_dirty = 0;
    }
    y += 44 + 22;
    if (button(x0, y, 48, 0, "REFILL BLOOD VIALS & QUICKSILVER BULLETS", B_OUTLINE, 13, 0.18f, NULL, false, x1 - x0) && g_supported) {
        char m[160]; int ok = prac_refill(m, sizeof m); if (ok != 2) show_msg(!ok, "%s", m);
    }
    y += 48 + 22;
    ImGui::SetCursorScreenPos(P(x0, y));
    ImGui::PushTextWrapPos(P(x1, 0).x);
    ImGui::PushFont(F_CGI, fsize(F_CGI, 14)); ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(C_MUTED));
    ImGui::TextWrapped("Toggles apply instantly and stay as you set them: loading a savestate, dying or travelling does not change them. Hotkeys can be assigned in Settings.");
    ImGui::PopStyleColor(); ImGui::PopFont(); ImGui::PopTextWrapPos();
}

/* ---- TELEPORT ---- */
static void point_save_here(int over) {   /* over: index to overwrite, -1 new point; the point becomes the active one */
    if (!V.pl.ok) { show_msg(1, "No hunter in the world"); toast(2, 0, "CANNOT SAVE POSITION", "No hunter in the world"); return; }
    const char *place = meta_place(V.pl.map_id);
    char mp[16]; _snprintf_s(mp, sizeof mp, _TRUNCATE, "m%02u_%02u", (V.pl.map_id >> 24) & 0xff, (V.pl.map_id >> 16) & 0xff);
    AcquireSRWLockExclusive(&g_lock);
    if (over < 0 || over >= g_npts) {
        if (g_npts >= 64) { ReleaseSRWLockExclusive(&g_lock); show_msg(1, "The list is full (64 points)"); return; }
        over = g_npts++;
        memset(&g_pts[over], 0, sizeof g_pts[over]);
        SYSTEMTIME t; GetLocalTime(&t);
        _snprintf_s(g_pts[over].name, sizeof g_pts[over].name, _TRUNCATE, "%s %02d:%02d", place[0] ? place : mp, t.wHour, t.wMinute);
    }
    Point &p = g_pts[over];
    p.map = V.pl.map_id; memcpy(p.pos, V.pl.pos, 12); p.yaw = V.pl.yaw;
    g_apt = over;
    points_save();
    char nm[64]; strncpy_s(nm, sizeof nm, p.name, _TRUNCATE);
    ReleaseSRWLockExclusive(&g_lock);
    show_msg(0, "Position saved");
    toast(4, 0, "POSITION SAVED", nm);
}
static void point_go(int i) {
    if (i < 0 || i >= g_npts) { show_msg(1, "No saved position"); toast(2, 0, "CANNOT TELEPORT", "No saved position yet"); return; }
    char why[200];
    g_apt = i; points_save();
    if (prac_teleport(g_pts[i].map, g_pts[i].pos, g_pts[i].yaw, why, sizeof why)) { show_msg(0, "Teleported to %s", g_pts[i].name); toast(4, 0, "POSITION LOADED", g_pts[i].name); }
    else { show_msg(1, "Cannot teleport: %s", why); toast(2, 0, "CANNOT TELEPORT", why); }
}
static void tab_teleport(void) {
    float x0 = 40, y = CT, cw = 340;
    float ch = 22 + lh(F_FELL, 13) / g_s + 16 + lh(F_CG, 16) / g_s + 16 + 2 * (lh(F_CG, 12) / g_s + lh(F_CG, 18) / g_s) + 10 + 16 + 46 + 16 + 5 * lh(F_CGI, 14) / g_s + 22;
    rect(P(x0, y), P(x0 + cw, y + ch), RGBA(0, 0, 0, 0.2f), hair(0.2f));
    float cx = x0 + 22, cy = y + 22;
    text(P(cx, cy), F_FELL, 13, C_GOLD, "CURRENT POSITION", 0.22f); cy += lh(F_FELL, 13) / g_s + 16;
    const char *place = V.pl.ok ? meta_place(V.pl.map_id) : "";
    char mp[48]; _snprintf_s(mp, sizeof mp, _TRUNCATE, "m%02u_%02u", (V.pl.map_id >> 24) & 0xff, (V.pl.map_id >> 16) & 0xff);
    text(P(cx, cy), F_CG, 16, C_SOFT, !V.pl.ok ? "No hunter in the world" : place[0] ? place : mp); cy += lh(F_CG, 16) / g_s + 16;
    char v[4][32];
    for (int k = 0; k < 3; ++k) _snprintf_s(v[k], 32, _TRUNCATE, V.pl.ok ? "%.2f" : "\xe2\x80\x94", V.pl.pos[k]);
    float deg = V.pl.yaw * 57.29578f; deg = fmodf(deg, 360); if (deg < 0) deg += 360;
    _snprintf_s(v[3], 32, _TRUNCATE, V.pl.ok ? "%.0f\xc2\xb0" : "\xe2\x80\x94", deg);
    for (int k = 0; k < 4; ++k) if (v[k][0] == '-') { char t[32]; _snprintf_s(t, 32, _TRUNCATE, "\xe2\x88\x92%s", v[k] + 1); strcpy_s(v[k], 32, t); }
    const char *lab[4] = {"X", "Y", "Z", "FACING"};
    float colw = (cw - 44 - 18) / 2, rh = lh(F_CG, 12) / g_s + lh(F_CG, 18) / g_s;
    for (int k = 0; k < 4; ++k) {
        float px = cx + (k % 2) * (colw + 18), py = cy + (k / 2) * (rh + 10);
        text(P(px, py), F_CG, 12, C_MUTED, lab[k], 0.16f);
        text(P(px, py + lh(F_CG, 12) / g_s), F_CG, 18, C_TEXT, v[k]);
    }
    cy += 2 * rh + 10 + 16;
    if (button(cx, cy, 46, 0, "SAVE THIS POSITION", B_PRIMARY, 13, 0.16f, NULL, false, cw - 44) && g_supported) point_save_here(-1);
    cy += 46 + 16;
    ImGui::SetCursorScreenPos(P(cx, cy)); ImGui::PushTextWrapPos(P(cx + cw - 44, 0).x);
    ImGui::PushFont(F_CGI, fsize(F_CGI, 14)); ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(C_MUTED));
    ImGui::TextWrapped("Teleport moves only the hunter, the world stays as it is. A point works only in the area where it was saved. Click a point to make it active for the position hotkeys.");
    ImGui::PopStyleColor(); ImGui::PopFont(); ImGui::PopTextWrapPos();
    /* saved points */
    float x1 = 1080, lx = x0 + cw + 36;
    text(P(lx, CT), F_FELL, 13, C_GOLD, "SAVED POINTS", 0.22f);
    float ly = CT + lh(F_FELL, 13) / g_s + 14;
    float rowh = 10 + lh(F_CG, 18) / g_s + lh(F_CG, 14) / g_s + 10;
    ImGui::SetCursorScreenPos(P(lx, ly));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##points", ImVec2((x1 - lx) * g_s + 10 * g_s, (CB - ly) * g_s), 0, g_modal ? ImGuiWindowFlags_NoScrollWithMouse : 0);
    ImVec2 save_o = g_o;
    float scroll = ImGui::GetScrollY();
    g_o = ImVec2(ImGui::GetWindowPos().x - lx * g_s, ImGui::GetWindowPos().y - ly * g_s - scroll);
    if (!g_npts) text(P(lx, ly + 10), F_CGI, 16, C_MUTED, "No points yet. Stand where you want to come back to and press Save this position.");
    int go = -1;
    for (int i = 0; i < g_npts; ++i) {
        const Point &p = g_pts[i];
        float ry = ly + i * (rowh + 1 + 6), rcy = ry + rowh / 2;
        float bw = 40, tw = text_w(F_FELL, 12, "TELEPORT", 0.16f) / g_s + 36, bx = x1 - bw - 10 - bw - 10 - tw;
        bool act = i == g_apt, rhov;
        if (hit(P(lx - 8, ry), P(bx - 10, ry + rowh), &rhov)) { g_apt = i; AcquireSRWLockExclusive(&g_lock); points_save(); ReleaseSRWLockExclusive(&g_lock); }
        if (act || rhov) rect(P(lx - 8, ry + 2), P(bx - 6, ry + rowh - 2), act ? hair(0.09f) : hair(0.04f), act ? hair(0.3f) : 0);
        float nameright = bx - 14;
        if (act) {
            float pw = text_w(F_CGM, 12, "ACTIVE", 0.12f) / g_s + 16, ph = lh(F_CGM, 12) / g_s + 4;
            rect(P(bx - 14 - pw, rcy - ph / 2), P(bx - 14, rcy + ph / 2), C_ACCENT);
            text(P(bx - 14 - pw + 8, rcy - ph / 2 + 2), F_CGM, 12, C_ONACC, "ACTIVE", 0.12f);
            nameright -= pw + 10;
        }
        ImGui::PushClipRect(P(lx, ry), P(nameright, ry + rowh), true);
        text(P(lx, ry + 10), F_CG, 18, C_BRIGHT, p.name);
        const char *pl = meta_place(p.map); char sub[160], a[3][24];
        for (int k = 0; k < 3; ++k) { _snprintf_s(a[k], 24, _TRUNCATE, "%.1f", p.pos[k]); if (a[k][0] == '-') { char t[24]; _snprintf_s(t, 24, _TRUNCATE, "\xe2\x88\x92%s", a[k] + 1); strcpy_s(a[k], 24, t); } }
        _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s \xc2\xb7 %s, %s, %s", pl[0] ? pl : "Unknown area", a[0], a[1], a[2]);
        text(P(lx, ry + 10 + lh(F_CG, 18) / g_s), F_CG, 14, C_MUTED, sub);
        ImGui::PopClipRect();
        if (button(bx, rcy - 20, 40, 18, "TELEPORT", B_OUTLINE, 12, 0.16f)) go = i;
        bool hov;
        ImVec2 ra = P(x1 - bw - 10 - bw, rcy - 20), rb = P(x1 - bw - 10, rcy + 20);
        if (hit(ra, rb, &hov)) { g_modal = 3; g_modal_arg = i; strncpy_s(g_edit, sizeof g_edit, p.name, _TRUNCATE); }
        rect(ra, rb, hov ? RGBA(255, 255, 255, 0.04f) : 0, hair(hov ? 0.45f : 0.25f));
        { ImVec2 c = ImVec2((ra.x + rb.x) / 2 - 8 * g_s, (ra.y + rb.y) / 2 - 8 * g_s); auto Q = [&](float x, float y_) { return ImVec2(c.x + x * g_s, c.y + y_ * g_s); };
          ImVec2 pen[5] = {Q(3, 13), Q(3.5f, 10.5f), Q(10.5f, 3.5f), Q(12.5f, 5.5f), Q(5.5f, 12.5f)}; DL()->AddPolyline(pen, 5, hov ? C_SOFT : C_MUTED, ImDrawFlags_Closed, 1.2f * g_s); }
        ImVec2 da = P(x1 - bw, rcy - 20), db = P(x1, rcy + 20);
        if (hit(da, db, &hov)) { g_modal = 4; g_modal_arg = i; }
        rect(da, db, hov ? RGBA(255, 255, 255, 0.04f) : 0, hair(hov ? 0.45f : 0.25f));
        { ImVec2 c = ImVec2((da.x + db.x) / 2, (da.y + db.y) / 2); float k = 4 * g_s;
          DL()->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), hov ? C_SOFT : C_MUTED, 1.2f * g_s); DL()->AddLine(ImVec2(c.x + k, c.y - k), ImVec2(c.x - k, c.y + k), hov ? C_SOFT : C_MUTED, 1.2f * g_s); }
        hline(P(lx, 0).x, P(x1, 0).x, P(0, ry + rowh).y, hair(0.12f));
    }
    ImGui::SetCursorScreenPos(P(lx, ly + g_npts * (rowh + 7) + 4));
    ImGui::Dummy(ImVec2(1, 1));
    ImGui::EndChild();
    ImGui::PopStyleColor();
    g_o = save_o;
    if (go >= 0) point_go(go);
}

/* ---- SETTINGS ---- */
static int key_conflict(int k) {
    for (int j = 0; j < K_COUNT; ++j)
        if (j != k && g_keys[k].vk && g_keys[j].vk == g_keys[k].vk && g_keys[j].shift == g_keys[k].shift && g_keys[j].ctrl == g_keys[k].ctrl && g_keys[j].alt == g_keys[k].alt) return j;
    return -1;
}
static void slider(float x, float cy, float w, int *v, int lo, int hi, const char *ini_key) {
    ImVec2 a = P(x, cy - 10), b = P(x + w, cy + 10);
    hit(a, b);
    if (ImGui::IsItemActive() && !g_modal) {
        float t = (ImGui::GetIO().MousePos.x - a.x) / (b.x - a.x); t = ImClamp(t, 0.0f, 1.0f);
        *v = lo + (int)(t * (hi - lo) + 0.5f);
    }
    if (ImGui::IsItemDeactivated()) ini_puti("overlay", ini_key, *v);
    float t = (*v - lo) / (float)(hi - lo), mx = a.x + (b.x - a.x) * t;
    DL()->AddRectFilled(ImVec2(a.x, P(0, cy).y - 2 * g_s), ImVec2(b.x, P(0, cy).y + 2 * g_s), hair(0.25f), 2 * g_s);
    DL()->AddRectFilled(ImVec2(a.x, P(0, cy).y - 2 * g_s), ImVec2(mx, P(0, cy).y + 2 * g_s), IM_COL32(0x6d, 0x8f, 0xb5, 255), 2 * g_s);
    DL()->AddCircleFilled(ImVec2(mx, P(0, cy).y), 8 * g_s, IM_COL32(0x6d, 0x8f, 0xb5, 255), 24);
}
static uint64_t g_free_bytes;
static void tab_settings(void) {
    float x0 = 40, x1 = 538, y = CT;
    text(P(x0, y), F_FELL, 13, C_GOLD, "HOTKEYS", 0.22f);
    y += lh(F_FELL, 13) / g_s + 8;
    for (int k = 0; k < K_COUNT; ++k) {
        float rh = 3 + 34 + 3, cy = y + rh / 2;
        int conf = key_conflict(k);
        textc(P(x0, 0).x, P(0, cy).y, F_CG, 17, C_BRIGHT, k_key_label[k]);
        float cw = text_w(F_FELL, 11, "CHANGE", 0.14f) / g_s + 28;
        if (g_capture == k) {
            if (button(x1 - cw, cy - 17, 34, 14, "CANCEL", B_QUIET, 11, 0.14f, NULL, false, cw)) g_capture = -1;
            const char *pk = "PRESS A KEY\xe2\x80\xa6";
            float kw = text_w(F_CGM, 13, pk, 0) / g_s + 20;
            keycap(x1 - cw - 14 - kw, cy, pk, 13, 10, 3, C_ACCENT, C_ONACC, 40);
        } else {
            if (button(x1 - cw, cy - 17, 34, 14, "CHANGE", B_QUIET, 11, 0.14f, NULL, false, cw)) g_capture = k;
            const char *t = key_label(g_keys[k]);
            float kw = ImMax(text_w(F_CGM, 13, t, 0) / g_s + 20, 40.0f);
            keycap(x1 - cw - 14 - kw, cy, t, 13, 10, 3, conf >= 0 ? C_ERR : hair(0.45f), conf >= 0 ? C_ERR : C_BRIGHT, 40);
            if (conf >= 0) {
                char m[80]; _snprintf_s(m, sizeof m, _TRUNCATE, "also: %s", k_key_label[conf]);
                float mw = text_w(F_CGI, 13, m, 0) / g_s;
                textc(P(x1 - cw - 14 - kw - 12 - mw, 0).x, P(0, cy).y, F_CGI, 13, C_ERR, m);
            }
        }
        hline(P(x0, 0).x, P(x1, 0).x, P(0, y + rh).y, hair(0.12f));
        y += rh + 1 + 3;
    }
    text(P(x0, y + 4), F_CGI, 14, C_MUTED, "Esc cancels, Delete clears a key.");
    /* general */
    x0 = 582; x1 = 1080; y = CT;
    text(P(x0, y), F_FELL, 13, C_GOLD, "GENERAL", 0.22f);
    y += lh(F_FELL, 13) / g_s + 8;
    struct { const char *l, *d; int on; } pr[4] = {
        {"Load active slot on death", "Off by default", V.opt.autoload_on_death},
        {"On-screen notifications", "A short line at the top after save, load and teleport", S.notify},
        {"Sound cues", "Short tones after save and load", V.opt.beeps},
        {"Pause game while the menu is open", "Game input is blocked either way", S.pause}};
    for (int i = 0; i < 4; ++i) {
        float rh = 8 + lh(F_CG, 17) / g_s + lh(F_CG, 14) / g_s + 8, cy = y + rh / 2;
        text(P(x0, y + 8), F_CG, 17, C_BRIGHT, pr[i].l);
        text(P(x0, y + 8 + lh(F_CG, 17) / g_s), F_CG, 14, C_MUTED, pr[i].d);
        if (toggle(x1 - 46, cy - 13, pr[i].on != 0)) {
            if (i == 0 || i == 2) { BbsOptions o = V.opt; if (i == 0) o.autoload_on_death = !o.autoload_on_death; else o.beeps = !o.beeps; if (g_supported) bbs_set_options(&o); V.opt = M.opt = o; }
            if (i == 1) { S.notify = !S.notify; ini_puti("overlay", "notifications", S.notify); }
            if (i == 3) { S.pause = !S.pause; ini_puti("overlay", "pause_game", S.pause); }
        }
        hline(P(x0, 0).x, P(x1, 0).x, P(0, y + rh).y, hair(0.12f));
        y += rh + 1 + 2;
    }
    for (int i = 0; i < 2; ++i) {   /* opacity, size */
        float rh = 10 + 20 + 10, cy = y + rh / 2;
        static int size_edit; static bool size_drag;   /* the size applies on release: the menu would rescale under the mouse */
        if (!size_drag) size_edit = S.size;
        int *v = i ? &size_edit : &S.opacity;
        textc(P(x0, 0).x, P(0, cy).y, F_CG, 17, C_BRIGHT, i ? "Menu size" : "Menu opacity");
        char pc[8]; _snprintf_s(pc, sizeof pc, _TRUNCATE, "%d%%", *v);
        textc(P(x1 - text_w(F_CG, 15, pc, 0) / g_s, 0).x, P(0, cy).y, F_CG, 15, C_MUTED, pc);
        slider(x1 - 44 - 16 - 180, cy, 180, v, i ? 60 : 50, i ? 150 : 100, i ? "size" : "opacity");
        if (i) { size_drag = ImGui::IsItemActive(); if (ImGui::IsItemDeactivated()) S.size = size_edit; }
        hline(P(x0, 0).x, P(x1, 0).x, P(0, y + rh).y, hair(0.12f));
        y += rh + 1;
    }
    y += 8;
    float rw = text_w(F_FELL, 11, "RESET SIZE & POSITION", 0.14f) / g_s + 28;
    if (button(x0, y, 32, 14, "RESET SIZE & POSITION", B_QUIET, 11, 0.14f, NULL, false, rw)) {
        S.size = 100; S.offx = S.offy = 0;
        ini_puti("overlay", "size", 100); ini_puti("overlay", "pos_x", 0); ini_puti("overlay", "pos_y", 0);
    }
    text(P(x0 + rw + 14, y + 8), F_CGI, 14, C_MUTED, "Drag the title to move, the edges to resize.");
    y += 32 + 16;
    text(P(x0, y), F_FELL, 13, C_GOLD, "STORAGE", 0.22f); y += lh(F_FELL, 13) / g_s + 8;
    float ow = text_w(F_FELL, 11, "OPEN", 0.14f) / g_s + 28;
    ImVec2 fa = P(x0, y), fb = P(x1 - ow - 10, y + 40);
    rect(fa, fb, RGBA(0, 0, 0, 0.35f), hair(0.3f));
    ImGui::PushClipRect(fa, fb, true);
    text(ImVec2(fa.x + 12 * g_s, (fa.y + fb.y - lh(F_CG, 15)) / 2), F_CG, 15, C_BRIGHT, V.slots_dir);
    ImGui::PopClipRect();
    if (button(x1 - ow, y, 40, 14, "OPEN", B_OUTLINE, 11, 0.14f, NULL, false, ow)) ShellExecuteA(NULL, "open", V.slots_dir, NULL, NULL, SW_SHOWNORMAL);
    y += 40 + 8;
    char st[160]; _snprintf_s(st, sizeof st, _TRUNCATE, "%.1f GB used \xc2\xb7 keeps at least %.0f GB free on the drive", V.used_bytes / 1073741824.0, V.opt.min_free_mb / 1024.0);
    text(P(x0, y), F_CG, 14, C_MUTED, st);
}

/* ---- ABOUT (opened from the header) ---- */
static int g_ab_n; static bool g_ab_alt;
static constexpr unsigned char k_ab_b[] = {0x3f, 0x37, 0x68, 0x2f, 0x37, 0x3a, 0x0c, 0x0d, 0x56};
struct AbT { char s[sizeof k_ab_b + 1]; };
static constexpr AbT ab_t() { AbT r{}; for (int i = 0; i < (int)sizeof k_ab_b; ++i) r.s[i] = (char)(k_ab_b[i] ^ (0x5a + i)); return r; }
static constexpr AbT k_ab_t = ab_t();
static void ab_shape(ImVec2 c, float r) {
    ImU32 red = IM_COL32(0x9e, 0x22, 0x35, 255);
    glow(ImVec2(c.x, c.y), r * 1.9f, IM_COL32(0xc2, 0x38, 0x4c, 0), 0.40f);
    ImDrawList *dl = DL(); const int N = 96; ImVec2 uv = dl->_Data->TexUvWhitePixel;
    float k = r / 15.0f;
    dl->PrimReserve(N * 3, N + 1);
    ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(c, uv, red);
    for (int i = 0; i < N; ++i) {
        float t = i * 6.2831853f / N, st = sinf(t);
        float x = 16 * st * st * st, y = 13 * cosf(t) - 5 * cosf(2 * t) - 2 * cosf(3 * t) - cosf(4 * t);
        dl->PrimWriteVtx(ImVec2(c.x + x * k, c.y - (y + 2.5f) * k * 0.92f), uv, red);
    }
    for (int i = 0; i < N; ++i) { dl->PrimWriteIdx(base); dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i)); dl->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % N)); }
}
static void tab_about(void) {
    float cx = 560, y = CT + 20;
    ImVec2 mc = P(cx, y + 55);
    if (hit(P(cx - 55, y), P(cx + 55, y + 110))) { if (++g_ab_n >= 5) { g_ab_alt = !g_ab_alt; g_ab_n = 0; } }
    if (g_ab_alt) {
        ab_shape(mc, 50 * g_s);
        const char *t = k_ab_t.s;
        float tw = text_w(F_CGM, 17, t, 0.04f) / g_s;
        text(P(cx - tw / 2, y + 55 - 6 - lh(F_CGM, 17) / g_s / 2), F_CGM, 17, C_ONACC, t, 0.04f);
    } else {
        glow(mc, 55 * g_s, IM_COL32(0xe9, 0xdc, 0xb5, 0), 0.45f);
        DL()->AddCircleFilled(mc, 30 * g_s, IM_COL32(0xec, 0xdc, 0xb0, 255), 64);
        DL()->AddCircleFilled(P(cx - 9, y + 47), 6 * g_s, IM_COL32(0xd6, 0xc4, 0x93, 178), 32);
        DL()->AddCircleFilled(P(cx + 8, y + 62), 8 * g_s, IM_COL32(0xd6, 0xc4, 0x93, 153), 32);
        DL()->AddCircleFilled(P(cx + 5, y + 44), 3.5f * g_s, IM_COL32(0xd6, 0xc4, 0x93, 178), 24);
    }
    y += 110 + 16;
    float w = text_w(F_FELL, 30, "PRACTICE TOOL", 0.2f) / g_s;
    text(P(cx - w / 2, y), F_FELL, 30, C_BRIGHT, "PRACTICE TOOL", 0.2f, 0.6f); y += lh(F_FELL, 30) / g_s + 16;
    const char *ver = "Version " OV_VERSION;
    w = text_w(F_CG, 16, ver, 0) / g_s; text(P(cx - w / 2, y), F_CG, 16, C_MUTED, ver); y += lh(F_CG, 16) / g_s + 16;
    hline(P(cx - 180, 0).x, P(cx - 14, 0).x, P(0, y + 3).y, hair(0.3f)); hline(P(cx + 14, 0).x, P(cx + 180, 0).x, P(0, y + 3).y, hair(0.3f));
    { ImVec2 c = P(cx, y + 3.5f); float r = 5 * g_s; DL()->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), C_GOLD); }
    y += 7 + 16;
    w = text_w(F_CG, 15, "CREATED BY", 0.2f) / g_s; text(P(cx - w / 2, y), F_CG, 15, C_MUTED, "CREATED BY", 0.2f); y += lh(F_CG, 15) / g_s + 16;
    w = text_w(F_FELL, 34, "billymoore", 0.08f) / g_s; text(P(cx - w / 2, y), F_FELL, 34, C_BRIGHT, "billymoore", 0.08f); y += lh(F_FELL, 34) / g_s + 20;
    float bw[3], tot = 0;
    for (int i = 0; i < 3; ++i) { bw[i] = text_w(F_FELL, 12, k_link_label[i], 0.16f) / g_s + 40; tot += bw[i]; }
    tot += 24;
    float bx = cx - tot / 2;
    for (int i = 0; i < 3; ++i) {
        if (button(bx, y, 44, 20, k_link_label[i], B_LINK, 12, 0.16f)) ShellExecuteA(NULL, "open", k_link_url[i], NULL, NULL, SW_SHOWNORMAL);
        bx += bw[i] + 12;
    }
}

/* ---- modal dialogs ---- */
static void modal(void) {
    if (!g_modal) return;
    float W = 560, H = 230, x0 = 280, y0 = 260;
    DL()->AddRectFilled(P(0, 0), P(1120, PH), RGBA(4, 7, 12, 0.55f));
    shadow(P(x0, y0 + 10), P(x0 + W, y0 + H + 10), 40 * g_s, 0.5f);
    rect(P(x0, y0), P(x0 + W, y0 + H), IM_COL32(12, 19, 28, 250), hair(0.3f));
    char title[96];
    bool rename = g_modal == 1 || g_modal == 3, slot = g_modal <= 2;
    if (slot) _snprintf_s(title, sizeof title, _TRUNCATE, "%s SLOT %s", rename ? "RENAME" : "DELETE", roman(g_modal_arg + 1));
    else _snprintf_s(title, sizeof title, _TRUNCATE, "%s POINT", rename ? "RENAME" : "DELETE");
    text(P(x0 + 32, y0 + 28), F_FELL, 15, C_GOLD, title, 0.22f);
    float y = y0 + 28 + lh(F_FELL, 15) / g_s + 20;
    bool ok = false, cancel = false;
    if (rename) {
        static int last_frame;   /* focus the field on the dialog's first frame */
        if (ImGui::GetFrameCount() != last_frame + 1) ImGui::SetKeyboardFocusHere();
        last_frame = ImGui::GetFrameCount();
        if (field("##edit", x0 + 32, y, W - 64, 44, g_edit, sizeof g_edit, 18, ImGuiInputTextFlags_AutoSelectAll, true)) ok = true;
        y += 44 + 12;
        text(P(x0 + 32, y), F_CGI, 14, C_MUTED, slot ? "Leave empty to use the place and time of the save." : "Enter to confirm, Esc to cancel.");
    } else {
        const char *what = slot ? slot_name(g_modal_arg) : g_pts[g_modal_arg].name;
        char m[160]; _snprintf_s(m, sizeof m, _TRUNCATE, "\xe2\x80\x9c%s\xe2\x80\x9d will be removed. This cannot be undone.", what);
        text(P(x0 + 32, y + 8), F_CG, 17, C_SOFT, m);
    }
    float by = y0 + H - 32 - 44, w2 = text_w(F_FELL, 13, "CANCEL", 0.16f) / g_s + 36;
    const char *okl = rename ? "SAVE" : "DELETE";
    float w1 = text_w(F_FELL, 13, okl, 0.16f) / g_s + 44;
    if (button(x0 + W - 32 - w1, by, 44, 22, okl, B_PRIMARY, 13, 0.16f, NULL, true)) ok = true;
    if (button(x0 + W - 32 - w1 - 10 - w2, by, 44, 18, "CANCEL", B_QUIET, 13, 0.16f, NULL, true)) cancel = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) cancel = true;
    if (!rename && ImGui::IsKeyPressed(ImGuiKey_Enter)) ok = true;
    if (ok) {
        if (g_modal == 1) { if (!bbs_rename(g_modal_arg + 1, g_edit)) show_msg(1, "The slot could not be renamed"); }
        else if (g_modal == 2) bbs_delete(g_modal_arg + 1);
        else if (g_modal == 3) { AcquireSRWLockExclusive(&g_lock); if (g_edit[0]) strncpy_s(g_pts[g_modal_arg].name, sizeof g_pts[0].name, g_edit, _TRUNCATE); points_save(); ReleaseSRWLockExclusive(&g_lock); }
        else if (g_modal == 4) { AcquireSRWLockExclusive(&g_lock); memmove(&g_pts[g_modal_arg], &g_pts[g_modal_arg + 1], (g_npts - g_modal_arg - 1) * sizeof(Point)); --g_npts; points_save(); ReleaseSRWLockExclusive(&g_lock); }
        g_modal = 0;
    } else if (cancel) g_modal = 0;
}

/* ---- notifications: one compact line at the top centre (the game's HUD is in the corners) ---- */
static void draw_toast(float W, float H) {
    Toast t; AcquireSRWLockShared(&g_lock); t = g_toast; ReleaseSRWLockShared(&g_lock);
    double age = now_s() - t.t;
    if (!t.t || age > 2.2) return;
    float alpha = (float)(age < 0.15 ? age / 0.15 : age > 1.9 ? (2.2 - age) / 0.3 : 1.0);
    auto A = [&](ImU32 c) { return (c & 0x00ffffff) | (ImU32)(((c >> 24) & 0xff) * alpha) << 24; };
    float keep = g_s; g_s = H / 900.0f;
    const char *num = t.slot ? roman(t.slot) : "";
    float pad = 14, gap = 10, nw = num[0] ? text_w(F_FELL, 17, num, 0) / g_s : t.kind == 2 ? 16 : 0;
    float tw = text_w(F_FELL, 12, t.title, 0.2f) / g_s, dw = t.detail[0] ? text_w(F_CG, 15, t.detail, 0) / g_s : 0;
    if (dw > 360) dw = 360;
    float w = pad + (nw ? nw + gap : 0) + tw + (dw ? gap + 6 + gap + dw : 0) + pad, h = 34;
    g_o = ImVec2(floorf((W - w * g_s) / 2), floorf(18 * g_s));
    shadow(P(0, 4), P(w, h + 4), 18 * g_s, 0.3f * alpha);
    rect(P(0, 0), P(w, h), A(IM_COL32(10, 16, 24, 214)), A(hair(0.25f)));
    float x = pad, cy = h / 2;
    if (t.kind == 2) { icon_error(P(x + 8, cy), 7.5f * g_s); x += 16 + gap; }
    else if (num[0]) { textc(P(x, 0).x, P(0, cy).y, F_FELL, 17, A(C_GOLD), num); x += nw + gap; }
    textc(P(x, 0).x, P(0, cy).y, F_FELL, 12, A(C_BRIGHT), t.title, 0.2f); x += tw;
    if (dw) {
        x += gap; DL()->AddCircleFilled(P(x + 2, cy), 1.6f * g_s, A(C_MUTED), 8); x += 6 + gap;
        ImGui::PushClipRect(P(x, 0), P(x + dw, h), true);
        textc(P(x, 0).x, P(0, cy).y, F_CG, 15, A(C_SOFT), t.detail);
        ImGui::PopClipRect();
    }
    if (t.kind == 0) DL()->AddRectFilled(P(pad, h - 1), P(w - pad, h + 1), A(C_ACCENT));
    g_s = keep;
}

/* ---- entry points ---- */
bool ui_wants_input(void) { return g_open != 0; }
bool ui_wants_frame(void) {
    if (g_open) return true;
    if (!S.notify) return false;
    return g_toast.t && now_s() - g_toast.t < 2.3;
}
void ui_on_key(int vk) {
    if (g_capture < 0) return;
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LWIN) return;
    if (vk == VK_ESCAPE) { g_capture = -1; g_block_until = now_s() + 0.5; return; }
    if (vk == VK_DELETE || vk == VK_BACK) { set_key(g_capture, "none"); g_capture = -1; g_block_until = now_s() + 0.5; return; }
    if (!*hk_vk_name(vk)) return;
    char t[48]; hk_format(vk, GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_MENU) < 0, t, sizeof t);
    set_key(g_capture, t);
    g_capture = -1;
    g_block_until = now_s() + 0.5;
}
void ui_build(float W, float H) {
    AcquireSRWLockShared(&g_lock); V = M; ReleaseSRWLockShared(&g_lock);
    g_s = H / 900.0f * S.size / 100.0f; g_id = 0;
    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::Begin("##overlay", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
    if (!g_open) {
        draw_toast(W, H);
        ImGui::End();
        return;
    }
    {   /* centred, moved by the user's offset; a part of the title always stays on screen */
        float base = H / 900.0f, ox = (W - 1120 * g_s) / 2 + S.offx * base, oy = (H - PH * g_s) / 2 + S.offy * base;
        ox = ImClamp(ox, -1000 * g_s, W - 120 * g_s); oy = ImClamp(oy, 0.0f, H - 70 * g_s);
        S.offx = (ox - (W - 1120 * g_s) / 2) / base; S.offy = (oy - (H - PH * g_s) / 2) / base;
        g_o = ImVec2(floorf(ox), floorf(oy));
    }
    draw_frame(W, H);
    if (!g_supported) {
        float y = CT;
        text(P(40, y), F_FELL, 13, C_GOLD, "NOT AVAILABLE", 0.22f);
        ImGui::SetCursorScreenPos(P(40, y + 30)); ImGui::PushTextWrapPos(P(1080, 0).x);
        ImGui::PushFont(F_CG, fsize(F_CG, 18)); ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(C_SOFT));
        ImGui::TextWrapped("%s", g_why);
        ImGui::PopStyleColor(); ImGui::PopFont(); ImGui::PopTextWrapPos();
    } else if (g_tab == 4) tab_about();
    else if (g_tab == 0) tab_savestates();
    else if (g_tab == 1) tab_practice();
    else if (g_tab == 2) tab_teleport();
    else if (g_tab == 3) tab_settings();
    if (!g_supported && g_tab == 4) tab_about();
    if (!g_modal) { g_typing = ImGui::GetIO().WantTextInput; ImGui::End(); return; }
    ImGui::End();
    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(W, H)); ImGui::SetNextWindowFocus();
    ImGui::Begin("##modal", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);   /* over the points list (a child window) */
    modal();
    g_typing = ImGui::GetIO().WantTextInput;
    ImGui::End();
}

/* ---- the overlay thread: hotkeys, pause, data for the panel, events -> toasts ---- */
static uint64_t dir_size(const char *dir, int depth) {
    char pat[MAX_PATH]; _snprintf_s(pat, sizeof pat, _TRUNCATE, "%s\\*", dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    uint64_t sum = 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 4) { char sub[MAX_PATH]; _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s\\%s", dir, fd.cFileName); sum += dir_size(sub, depth + 1); } }
        else sum += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return sum;
}
static uint64_t file_stamp(const char *p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!p[0] || !GetFileAttributesExA(p, GetFileExInfoStandard, &a)) return 0;
    return (((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime) ^ a.nFileSizeLow;
}
static void clear_thumb(int slot) { ovtex_set(OVTEX_SLOT0 + slot, NULL, 0, 0); ovtex_set(OVTEX_MINI0 + slot, NULL, 0, 0); }
static void load_thumb(int slot, const char *path) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { clear_thumb(slot); return; }
    static uint8_t buf[54 + META_THUMB_BYTES], rgba[META_THUMB_W * META_THUMB_H * 4];
    DWORD got = 0; ReadFile(f, buf, sizeof buf, &got, NULL); CloseHandle(f);
    /* 24-bit bottom-up BMP of any size up to META_THUMB_W x H (v0.1.0 slots: 256x144) */
    int32_t w = got >= 54 ? *(int32_t *)(buf + 18) : 0, h = got >= 54 ? *(int32_t *)(buf + 22) : 0;
    size_t stride = ((size_t)w * 3 + 3) & ~(size_t)3;
    if (w <= 0 || h <= 0 || w > META_THUMB_W || h > META_THUMB_H || *(uint16_t *)(buf + 28) != 24 || got < 54 + stride * h) { clear_thumb(slot); return; }
    for (int y = 0; y < h; ++y) {
        const uint8_t *s = buf + 54 + (size_t)(h - 1 - y) * stride;
        uint8_t *o = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) { o[4 * x] = s[3 * x + 2]; o[4 * x + 1] = s[3 * x + 1]; o[4 * x + 2] = s[3 * x]; o[4 * x + 3] = 255; }
    }
    ovtex_set(OVTEX_SLOT0 + slot, rgba, w, h);
    static uint8_t mini[(META_THUMB_W / 2) * (META_THUMB_H / 2) * 4];   /* box average to about 96 px wide */
    int k = w / 96 > 1 ? w / 96 : 1, mw = w / k, mh = h / k;
    for (int y = 0; y < mh; ++y) for (int x = 0; x < mw; ++x) for (int c = 0; c < 4; ++c) {
        int sum = 0;
        for (int j = 0; j < k; ++j) for (int i = 0; i < k; ++i) sum += rgba[((size_t)(y * k + j) * w + x * k + i) * 4 + c];
        mini[((size_t)y * mw + x) * 4 + c] = (uint8_t)(sum / (k * k));
    }
    ovtex_set(OVTEX_MINI0 + slot, mini, mw, mh);
}
static void refresh(int full) {
    static Model m;
    if (g_supported) {
        bbs_list(m.slot, BBS_SLOTS); bbs_status(&m.st); bbs_get_options(&m.opt);
        prac_read(&m.pl);
        for (int i = 0; i < PR_COUNT; ++i) m.flags[i] = prac_flag(i);
        for (int i = 0; i < BBS_SLOTS; ++i) {
            uint64_t st = m.slot[i].used ? file_stamp(m.slot[i].thumb) : 0;
            if (st != m.thumb_stamp[i]) { m.thumb_stamp[i] = st; if (st) load_thumb(i, m.slot[i].thumb); else clear_thumb(i); }
        }
        core_slots_dir(m.slots_dir, sizeof m.slots_dir);
        if (full) m.used_bytes = dir_size(m.slots_dir, 0);
    }
    AcquireSRWLockExclusive(&g_lock); M = m; ReleaseSRWLockExclusive(&g_lock);
}
static void on_events(void) {
    static uint64_t seen; static int first = 1;
    BbsEvent ev[16]; int n = bbs_events(seen, ev, 16);
    if (first) { first = 0; if (n) seen = ev[n - 1].seq; return; }
    for (int i = 0; i < n; ++i) {
        seen = ev[i].seq;
        const BbsEvent &e = ev[i];
        BbsSlotInfo si; int has = e.slot >= 1 && bbs_slot_info(e.slot, &si);
        const char *nm = has ? si.name : "Empty slot";
        char why[200];
        switch (e.type) {
        case BBS_EV_SAVED: toast(0, e.slot, "STATE SAVED", nm); show_msg(0, "Saved to slot %s", roman(e.slot)); break;
        case BBS_EV_LOADED: toast(0, e.slot, "STATE LOADED", nm); show_msg(0, "Slot %s loaded", roman(e.slot)); break;
        case BBS_EV_ACTIVE: toast(1, e.slot, "ACTIVE SLOT", nm); break;
        case BBS_EV_REFUSED:
            friendly(e.text, why, sizeof why);
            toast(2, e.slot, e.op == BBS_OP_SAVE ? "CANNOT SAVE" : e.op == BBS_OP_DELETE ? "CANNOT DELETE" : "CANNOT LOAD", why);
            show_msg(1, "%s: %s", e.op == BBS_OP_SAVE ? "Cannot save" : e.op == BBS_OP_DELETE ? "Cannot delete" : "Cannot load", why);
            break;
        case BBS_EV_LOST: toast(2, e.slot, "SLOT LOST", "The slot could not be written: the disk is full"); break;
        case BBS_EV_DELETED: show_msg(0, "Slot %s deleted", roman(e.slot)); break;
        }
    }
}
static int game_focused(void) {
    HWND w = GetForegroundWindow(); DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}
int ov_keys_blocked(void) {
    if (!g_open) return 0;
    return g_capture >= 0 || g_modal == 1 || g_modal == 3 || g_typing || now_s() < g_block_until;
}
int ov_is_open(void) { return g_open != 0; }
static void set_open(int on);
void ov_command(const char *w) {   /* tests: "0".."4" open on that tab, "close", "sel N" select slot N */
    if (!strcmp(w, "close")) { if (g_open) set_open(0); return; }
    if (!strncmp(w, "sel ", 4)) { g_sel = atoi(w + 4) - 1; return; }
    g_tab = atoi(w) % 5; if (!g_open) { set_open(1); refresh(1); }
}
static void set_open(int on) {
    InterlockedExchange(&g_open, on);
    g_capture = -1; g_modal = 0; g_echo_dirty = g_ins_dirty = 0;
    if (g_supported) core_ov_capture(on, on && S.pause);
    ov_log("overlay %s", on ? "opened" : "closed");
}
static DWORD WINAPI ov_thread(LPVOID) {
    int prev[K_COUNT] = {0};
    double last_full = 0, last_ref = 0, last_redraw = 0;
    refresh(1);
    for (;;) {
        Sleep(10);
        double t = now_s();
        int focused = game_focused();
        int blocked = ov_keys_blocked();
        if (focused) {   /* presses while typing or assigning a key are read and dropped */
            if (hk_pressed(&g_keys[K_TOGGLE], &prev[K_TOGGLE]) && !blocked) { set_open(!g_open); refresh(1); last_full = t; }
            if (hk_pressed(&g_keys[K_POS_SAVE], &prev[K_POS_SAVE]) && !blocked && g_supported) {   /* like F5: overwrites the active point */
                PracPlayer pl; prac_read(&pl);
                AcquireSRWLockExclusive(&g_lock); M.pl = pl; ReleaseSRWLockExclusive(&g_lock);
                V.pl = pl;
                point_save_here(g_apt);
            }
            if (hk_pressed(&g_keys[K_POS_LOAD], &prev[K_POS_LOAD]) && !blocked && g_supported) point_go(g_apt);
            for (int i = 0; i < PR_COUNT; ++i) if (hk_pressed(&g_keys[K_IMMORTAL + i], &prev[K_IMMORTAL + i]) && !blocked && g_supported) {
                static const char *nm[PR_COUNT] = {"IMMORTAL", "INFINITE STAMINA", "INFINITE RESOURCES", "STEALTH", "SILENT"};
                int on = !prac_flag(i);
                if (prac_set_flag(i, on)) toast(3, 0, nm[i], on ? "On" : "Off");
            }
        }
        if (g_supported) {
            on_events();
            { BbsStatus st; bbs_status(&st); if (st.state == BBS_READY) prac_tick(); }
            { char m[160], note[128]; int r = prac_refill_poll(m, sizeof m, note, sizeof note); if (r) { show_msg(r < 0, "%s", m); ov_log("%s", note); } }
            int want_hold = g_open && S.pause;
            core_ov_capture(g_open, g_open && (S.pause || ov_keys_blocked() || g_modal));   /* no pause: the game keeps its controls */
            core_ov_hold(want_hold);
            if (want_hold && core_ov_held() && t - last_redraw > 1 / 60.0) {
                core_ov_redraw();
                last_redraw = t;
            }
        }
        if (g_open && t - last_ref > 0.25) { refresh(t - last_full > 5); if (t - last_full > 5) last_full = t; last_ref = t; }
        else if (!g_open && t - last_ref > 1.0) { if (g_supported) { BbsStatus st; bbs_status(&st); AcquireSRWLockExclusive(&g_lock); M.st = st; ReleaseSRWLockExclusive(&g_lock); } last_ref = t; }
    }
    return 0;
}
void ov_start(const OvConfig *c) {
    g_supported = c->supported;
    strncpy_s(g_why, sizeof g_why, c->why ? c->why : "", _TRUNCATE);
    strncpy_s(g_dir, sizeof g_dir, c->dir, _TRUNCATE);
    strncpy_s(g_ini, sizeof g_ini, c->ini, _TRUNCATE);
    _snprintf_s(g_points_ini, sizeof g_points_ini, _TRUNCATE, "%s\\teleport.ini", g_dir);
    load_settings();
    points_load();
    HANDLE h = CreateThread(NULL, 1 << 20, ov_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    ov_log("overlay: thread started (%s), toggle %s, %d teleport points", g_supported ? "full" : "not supported: message only", g_keys[K_TOGGLE].text, g_npts);
}
