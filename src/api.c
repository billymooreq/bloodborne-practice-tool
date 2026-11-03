/* api.c: the savestate module's interface for the in-game overlay (bbs_api.h). Requests go into a small
 * queue that the DLL's worker thread empties (bbstate.c worker loop: api_take); the engine reports the
 * start and the end of every save/load/delete here (api_begin/api_end), whoever asked for it, so status
 * and events cover hotkeys and cmd.txt too. No game hooks here. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "bbs_core.h"
#include "meta.h"

#define QCAP 8
#define EVCAP 64
static SRWLOCK g_lock = SRWLOCK_INIT;
static struct { int op, slot; } g_q[QCAP];
static int g_qn;
static BbsStatus g_st;
static BbsEvent g_ev[EVCAP];
static uint64_t g_seq;
static LARGE_INTEGER g_qpf;

void api_init(void) { QueryPerformanceFrequency(&g_qpf); }
static int slot_ok(int *slot) { if (!*slot) *slot = core_active(); return *slot >= 1 && *slot <= BBS_SLOTS; }
static int enqueue(int op, int slot) {
    if (!slot_ok(&slot)) return 0;
    AcquireSRWLockExclusive(&g_lock);
    int ok = g_qn < QCAP;
    if (ok) { g_q[g_qn].op = op; g_q[g_qn].slot = slot; ++g_qn; g_st.queued = g_qn; }
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}
int bbs_save(int slot) { return enqueue(BBS_OP_SAVE, slot); }
int bbs_load(int slot) { return enqueue(BBS_OP_LOAD, slot); }
int bbs_delete(int slot) { return enqueue(BBS_OP_DELETE, slot); }
int api_take(int *op, int *slot) {
    AcquireSRWLockExclusive(&g_lock);
    int ok = g_qn > 0;
    if (ok) { *op = g_q[0].op; *slot = g_q[0].slot; memmove(g_q, g_q + 1, (size_t)(g_qn - 1) * sizeof *g_q); --g_qn; g_st.queued = g_qn; }
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

void api_event(int type, int slot, int op, const char *fmt, ...) {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    AcquireSRWLockExclusive(&g_lock);
    ++g_seq;
    BbsEvent *e = &g_ev[g_seq % EVCAP];
    e->seq = g_seq; e->type = type; e->slot = slot; e->op = op;
    e->time_ms = g_qpf.QuadPart ? t.QuadPart * 1000.0 / g_qpf.QuadPart : 0;
    va_list ap; va_start(ap, fmt); _vsnprintf_s(e->text, sizeof e->text, _TRUNCATE, fmt, ap); va_end(ap);
    g_st.last_event = g_seq;
    ReleaseSRWLockExclusive(&g_lock);
}
void api_begin(int op, int slot) {
    AcquireSRWLockExclusive(&g_lock);
    g_st.state = op == BBS_OP_SAVE ? BBS_SAVING : op == BBS_OP_LOAD ? BBS_LOADING : BBS_DELETING;
    ReleaseSRWLockExclusive(&g_lock);
    (void)slot;
}
void api_end(int op, int slot, int ok, const char *reason, double pause_ms) {
    static const char *const what[] = {"", "Save", "Load", "Delete"};
    AcquireSRWLockExclusive(&g_lock);
    g_st.state = BBS_READY; g_st.last_op = op; g_st.last_slot = slot; g_st.last_ok = ok; g_st.last_pause_ms = pause_ms;
    strncpy_s(g_st.last_reason, sizeof g_st.last_reason, ok || !reason ? "" : reason, _TRUNCATE);
    ReleaseSRWLockExclusive(&g_lock);
    if (!ok) api_event(BBS_EV_REFUSED, slot, op, "%s refused: %s", what[op & 3], reason ? reason : "");
    else if (op == BBS_OP_SAVE) api_event(BBS_EV_SAVED, slot, op, "Saved to slot %d", slot);
    else if (op == BBS_OP_LOAD) api_event(BBS_EV_LOADED, slot, op, "Slot %d loaded", slot);
    else api_event(BBS_EV_DELETED, slot, op, "Slot %d deleted", slot);
}

void bbs_status(BbsStatus *out) {
    AcquireSRWLockShared(&g_lock); *out = g_st; ReleaseSRWLockShared(&g_lock);
    out->active_slot = core_active();
}
int bbs_events(uint64_t after, BbsEvent *out, int cap) {
    int n = 0;
    AcquireSRWLockShared(&g_lock);
    uint64_t first = g_seq > EVCAP ? g_seq - EVCAP + 1 : 1;
    if (after + 1 > first) first = after + 1;
    for (uint64_t s = first; s <= g_seq && n < cap; ++s) out[n++] = g_ev[s % EVCAP];
    ReleaseSRWLockShared(&g_lock);
    return n;
}

int bbs_active(void) { return core_active(); }
int bbs_set_active(int slot) {
    if (slot < 1 || slot > BBS_SLOTS) return 0;
    core_set_active(slot);
    api_event(BBS_EV_ACTIVE, slot, BBS_OP_NONE, "Slot %d", slot);
    return 1;
}
int bbs_slot_info(int slot, BbsSlotInfo *o) {
    memset(o, 0, sizeof *o);
    if (slot < 1 || slot > BBS_SLOTS) return 0;
    o->slot = slot; o->used = core_slot_state(slot); o->damaged = core_slot_damaged(slot);
    if (!o->used) return 0;
    char dir[MAX_PATH]; core_slot_dir(slot, dir, sizeof dir);
    SlotMeta m;
    if (meta_read(dir, &m, o->name, sizeof o->name, o->thumb, sizeof o->thumb)) {
        strcpy_s(o->location, sizeof o->location, m.location); strcpy_s(o->map, sizeof o->map, m.map); strcpy_s(o->created, sizeof o->created, m.created);
        o->play_time_ms = m.play_time_ms; o->level = m.level; o->insight = m.insight; o->hp = m.hp; o->hp_max = m.hp_max; o->echoes = m.echoes; o->same_run_only = m.same_run_only; memcpy(o->hunter, m.hunter, sizeof o->hunter);
        memcpy(o->pos, m.pos, sizeof o->pos);
        if (!o->name[0]) meta_default_name(&m, o->name, sizeof o->name);
    } else _snprintf_s(o->name, sizeof o->name, _TRUNCATE, "Slot %d", slot);
    return 1;
}
int bbs_list(BbsSlotInfo *out, int cap) {
    int n = 0;
    for (int s = 1; s <= BBS_SLOTS && n < cap; ++s) bbs_slot_info(s, &out[n++]);
    return n;
}
int bbs_rename(int slot, const char *name) {
    if (slot < 1 || slot > BBS_SLOTS || !core_slot_state(slot)) return 0;
    char dir[MAX_PATH], clean[64]; core_slot_dir(slot, dir, sizeof dir);
    int k = 0;
    for (const char *p = name ? name : ""; *p && k < (int)sizeof clean - 1; ++p) clean[k++] = (*p == '\r' || *p == '\n') ? ' ' : *p;
    clean[k] = 0;
    if (!meta_rename(dir, clean)) return 0;
    api_event(BBS_EV_RENAMED, slot, BBS_OP_NONE, "Slot %d renamed", slot);
    return 1;
}
void bbs_get_options(BbsOptions *o) { core_get_options(o); }
int bbs_set_options(const BbsOptions *o) { core_set_options(o); return 1; }
