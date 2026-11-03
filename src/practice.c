/* practice.c: practice tools on the game's memory (Bloodborne 1.09 in bbport 1.9.5; see practice.h).
 * Toggles are the game's own debug flags in its data section: they are part of guest memory, so a
 * savestate captures them and a load brings back the state they had at the save. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "practice.h"

#define ROOT_A    0x805543ED0ull     /* A = [ROOT_A]; PlayerIns = [A] */
#define GAMEDATA  0x80553B130ull     /* GameDataMan; +8 PlayerGameData */
#define VT_PLAYER 0x80538F310ull
#define VT_PROXY  0x8053A7960ull
#define VT_PHYS   0x8053356F0ull
#define DBG_FLAGS 0x80553E888ull     /* PlayerNoDead (+0), ... */

/* Where each toggle lives (names from the game's own debug menu, WorldChrManDbg / the character's data module):
 *  immortal  "Player No Dead"      global byte DBG_FLAGS+0
 *  stealth   "Player Hide"         global byte DBG_FLAGS+6
 *  silent    "Player Silence"      global byte DBG_FLAGS+7
 *  stamina   "<No Stamina Consume" bit 0x80 of the word at datamod+0x200 (the player only; the global
 *            "All No Stamina Consume" DBG_FLAGS+2 would give every enemy endless stamina too)
 *  items     "<No Goods Consume"   bit 0x200 of the same word (vials, bullets, every consumable)
 * The data module is made anew when the player respawns or an area loads, and a savestate load brings back
 * whatever the slot had: prac_tick puts every toggle back to what the user chose (the toggles do not travel
 * with savestates). */
#define DM_FLAGS 0x200
static const struct { uint64_t global; uint16_t dm_bit; } k_flag[PR_COUNT] = {
    {DBG_FLAGS + 0, 0}, {0, 0x80}, {0, 0x200}, {DBG_FLAGS + 6, 0}, {DBG_FLAGS + 7, 0},
};
static volatile LONG g_want[PR_COUNT] = {-1, -1, -1, -1, -1};   /* -1: not chosen yet (take the game's state) */
#define MAX_BULLETS 0x805593034ull   /* the game's current maximum of quicksilver bullets (runes change it), 0: param */
#define MAX_VIALS   0x805593038ull
#define INV_ENTRIES 0x6c0            /* PlayerGameData: goods/weapons entries {handle, item, count, order}, 16 B */
#define INV_COUNT   1984
static uint64_t rq(uint64_t a) { return *(volatile uint64_t *)a; }
static int player(uint64_t *ins, uint64_t *pgd, uint64_t *phys) {
    __try {
        uint64_t A = rq(ROOT_A), i = A ? rq(A) : 0;
        if (!i || rq(i) != VT_PLAYER) return 0;
        uint64_t g = rq(GAMEDATA), d = g ? rq(g + 8) : 0, m = rq(i + 0x3B0), p = m ? rq(m + 0x68) : 0;
        if (!d || !p || rq(p) != VT_PHYS) return 0;
        if (ins) *ins = i; if (pgd) *pgd = d; if (phys) *phys = p;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
int prac_read(PracPlayer *p) {
    memset(p, 0, sizeof *p);
    uint64_t ins, pgd, phys;
    if (!player(&ins, &pgd, &phys)) return 0;
    __try {
        p->map_id = *(volatile uint32_t *)(ins + 0x3f8);
        memcpy(p->pos, (void *)(phys + 0x1E0), 12);
        p->yaw = *(volatile float *)(phys + 0x1D4);
        p->hp = *(volatile int32_t *)(pgd + 0x14); p->hp_max = *(volatile int32_t *)(pgd + 0x18);
        p->insight = *(volatile int32_t *)(pgd + 0x84); p->level = *(volatile int32_t *)(pgd + 0x90);
        p->echoes = *(volatile uint32_t *)(pgd + 0x94);
        p->ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { p->ok = 0; }
    return p->ok;
}
static uint64_t datamod(void) {
    uint64_t ins;
    if (!player(&ins, NULL, NULL)) return 0;
    __try { return rq(rq(ins + 0x3B0) + 0x20); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int game_flag(int w, int *on) {   /* 0: not readable now (no player for the data module bits) */
    __try {
        if (k_flag[w].global) { *on = *(volatile uint8_t *)k_flag[w].global != 0; return 1; }
        uint64_t dm = datamod();
        if (!dm) return 0;
        *on = (*(volatile uint16_t *)(dm + DM_FLAGS) & k_flag[w].dm_bit) != 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int write_flag(int w, int on) {
    __try {
        if (k_flag[w].global) { *(volatile uint8_t *)k_flag[w].global = (uint8_t)(on != 0); return 1; }
        uint64_t dm = datamod();
        if (!dm) return 0;
        volatile uint16_t *f = (volatile uint16_t *)(dm + DM_FLAGS);
        *f = (uint16_t)(on ? *f | k_flag[w].dm_bit : *f & ~k_flag[w].dm_bit);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
int prac_flag(int w) {
    if (w < 0 || w >= PR_COUNT) return 0;
    if (g_want[w] >= 0) return g_want[w];
    int on = 0; game_flag(w, &on);
    return on;
}
int prac_set_flag(int w, int on) {
    if (w < 0 || w >= PR_COUNT) return 0;
    InterlockedExchange(&g_want[w], on != 0);
    write_flag(w, on);   /* the data module bits wait for the hunter if there is none now (prac_tick) */
    return 1;
}
void prac_tick(void) {
    for (int w = 0; w < PR_COUNT; ++w) {
        int on;
        if (!game_flag(w, &on)) continue;
        if (g_want[w] < 0) { InterlockedExchange(&g_want[w], on); continue; }   /* first sight: the game's own state (the port's cheat options) */
        if (on != g_want[w]) write_flag(w, g_want[w]);
    }
}
int prac_set_echoes(uint32_t v) {
    uint64_t pgd;
    if (!player(NULL, &pgd, NULL)) return 0;
    __try { *(volatile uint32_t *)(pgd + 0x94) = v; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
int prac_set_insight(int v) {
    uint64_t pgd;
    if (!player(NULL, &pgd, NULL)) return 0;
    __try { *(volatile int32_t *)(pgd + 0x84) = v; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* Refill. A goods entry at 0 is dropped from the inventory, and only the game's own "add item" function
 * (the one pickups call) makes a new one: add_item(PlayerGameData+0x1D0, 0x40000000 | goods, id, count, 1, 1, 0).
 * It is guest code, so it runs on the game's render thread at the frame boundary (prac_frame from bbs_pre). */
#define FN_ADD_ITEM 0x8014cdfd0ull
#define PGD_ITEMS   0x1d0
#define GOODS       0x40000000u
extern int64_t sysv_call7(uint64_t fn, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g);
static const uint32_t k_res_id[2] = {1000, 900};   /* Blood Vial, Quicksilver Bullets */
static volatile LONG g_give;                       /* 0 idle, 1 asked, 2 on the render thread, 3 done */
static int g_give_want[2], g_give_got[2];          /* want 0: nothing to add; got: count afterwards, -1 none */
static ULONGLONG g_give_t;
static char g_give_note[96];
static volatile uint32_t *inv_find(uint64_t pgd, uint32_t item) {
    for (int i = 0; i < INV_COUNT; ++i) {
        volatile uint32_t *e = (volatile uint32_t *)(pgd + INV_ENTRIES + 16ull * i);
        if (e[1] == item) return e;
    }
    return NULL;
}
static void res_max(int m[2]) {
    m[1] = *(volatile int32_t *)MAX_BULLETS; m[0] = *(volatile int32_t *)MAX_VIALS;
    for (int k = 0; k < 2; ++k) if (m[k] <= 0 || m[k] > 999) m[k] = 20;
}
static void refill_msg(char *msg, size_t n, const int got[2]) {
    if (got[0] < 0 && got[1] < 0) _snprintf_s(msg, n, _TRUNCATE, "Blood Vials and Quicksilver Bullets could not be added");
    else if (got[0] < 0) _snprintf_s(msg, n, _TRUNCATE, "Bullets refilled to %d. Blood Vials could not be added", got[1]);
    else if (got[1] < 0) _snprintf_s(msg, n, _TRUNCATE, "Blood Vials refilled to %d. Bullets could not be added", got[0]);
    else _snprintf_s(msg, n, _TRUNCATE, "Refilled: %d Blood Vials, %d Quicksilver Bullets", got[0], got[1]);
}
int prac_refill(char *msg, size_t n) {
    uint64_t pgd;
    if (!player(NULL, &pgd, NULL)) { _snprintf_s(msg, n, _TRUNCATE, "No hunter in the world"); return 0; }
    if (g_give) { _snprintf_s(msg, n, _TRUNCATE, "Refilling\xe2\x80\xa6"); return 2; }
    int m[2], got[2] = {-1, -1}, missing = 0;
    __try {
        res_max(m);
        for (int k = 0; k < 2; ++k) {
            volatile uint32_t *e = inv_find(pgd, GOODS | k_res_id[k]);
            if (e) { e[2] = (uint32_t)m[k]; got[k] = m[k]; }
            else ++missing;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { _snprintf_s(msg, n, _TRUNCATE, "The inventory could not be read"); return 0; }
    if (!missing) { refill_msg(msg, n, got); return 1; }
    for (int k = 0; k < 2; ++k) { g_give_want[k] = got[k] < 0 ? m[k] : 0; g_give_got[k] = got[k]; }
    g_give_t = GetTickCount64();
    InterlockedExchange(&g_give, 1);
    _snprintf_s(msg, n, _TRUNCATE, "Refilling\xe2\x80\xa6");
    return 2;
}
void prac_frame(void) {
    if (InterlockedCompareExchange(&g_give, 2, 1) != 1) return;
    uint64_t pgd;
    int calls = 0;
    if (player(NULL, &pgd, NULL)) {
        for (int k = 0; k < 2; ++k) {
            if (!g_give_want[k]) continue;
            __try {
                if (!inv_find(pgd, GOODS | k_res_id[k])) {
                    sysv_call7(FN_ADD_ITEM, pgd + PGD_ITEMS, GOODS, k_res_id[k], (uint64_t)g_give_want[k], 1, 1, 0);
                    ++calls;
                }
                volatile uint32_t *e = inv_find(pgd, GOODS | k_res_id[k]);
                if (e) { if ((int)e[2] != g_give_want[k]) e[2] = (uint32_t)g_give_want[k]; g_give_got[k] = (int)e[2]; }
            } __except (EXCEPTION_EXECUTE_HANDLER) { g_give_got[k] = -1; }
        }
    }
    wchar_t *tn = NULL; char th[32] = "?";
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &tn)) && tn) { WideCharToMultiByte(CP_UTF8, 0, tn, -1, th, sizeof th, NULL, NULL); LocalFree(tn); }
    _snprintf_s(g_give_note, sizeof g_give_note, _TRUNCATE, "refill: %d add_item call(s) on thread '%s' (tid %lu)", calls, th, GetCurrentThreadId());
    InterlockedExchange(&g_give, 3);
}
int prac_refill_poll(char *msg, size_t n, char *note, size_t nn) {
    LONG s = g_give;
    if (s == 3) {
        refill_msg(msg, n, g_give_got);
        _snprintf_s(note, nn, _TRUNCATE, "%s", g_give_note);
        int ok = g_give_got[0] >= 0 || g_give_got[1] >= 0;
        InterlockedExchange(&g_give, 0);
        return ok ? 1 : -1;
    }
    if (s == 1 && GetTickCount64() - g_give_t > 3000 && InterlockedCompareExchange(&g_give, 0, 1) == 1) {
        _snprintf_s(msg, n, _TRUNCATE, "Cannot refill right now: the game is not running frames");
        _snprintf_s(note, nn, _TRUNCATE, "refill: render thread did not take the request in 3 s");
        return -1;
    }
    return 0;
}
int prac_teleport(uint32_t map_id, const float pos[3], float yaw, char *why, size_t n) {
    uint64_t ins, phys;
    if (!player(&ins, NULL, &phys)) { _snprintf_s(why, n, _TRUNCATE, "no hunter in the world"); return 0; }
    __try {
        uint32_t now = *(volatile uint32_t *)(ins + 0x3f8);
        if ((now & 0xffff0000u) != (map_id & 0xffff0000u)) { _snprintf_s(why, n, _TRUNCATE, "the point is in another area \xe2\x80\x94 travel there first"); return 0; }
        uint64_t A = rq(ROOT_A), proxy = rq(rq(rq(A + 0x18) + 0x80) + 0xF8);
        if (proxy && rq(proxy) == VT_PROXY) memcpy((void *)(proxy + 0x70), pos, 12);
        memcpy((void *)(phys + 0x1C0), pos, 12); memcpy((void *)(phys + 0x1E0), pos, 12); memcpy((void *)(phys + 0x1F0), pos, 12);
        *(volatile float *)(phys + 0x1D4) = yaw;
    } __except (EXCEPTION_EXECUTE_HANDLER) { _snprintf_s(why, n, _TRUNCATE, "the hunter is not ready"); return 0; }
    return 1;
}
