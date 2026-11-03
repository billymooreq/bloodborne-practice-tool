/* vsync.c: guest mutexes, condition variables, mutex attributes and rwlocks implemented in the DLL
 * (early mode: the import resolver hands these to the eboot instead of runtime_mutex.c/rwlock.c).
 *
 * A guest cell holds a handle VS_TAG | type << 40 | generation << 24 | index into one table of 64-byte
 * entries; all state is plain data (no OS objects): waits use WaitOnAddress on words of the table or
 * of the waiting thread's record. A slot keeps a copy of the table; a load puts it back, moves the
 * holds of the live threads to whatever handles their cells contain now, and wakes every waiter (an
 * epoch counter makes lock waiters re-read their cell; condition waits return as spurious wakeups).
 *
 * Semantics follow runtime_mutex.c / runtime_rwlock.c: static initializers (mutex cell 0/1, cond 0/1,
 * rwlock 0), destroyed values (mutex 2, cond 0, rwlock 1), Orbis error codes 0x8002xxxx for sce*
 * functions and positive errno values for the POSIX ones; mutex types 1 errorcheck, 2 recursive,
 * 3 normal, 4 adaptive (= errorcheck). Condition variables keep a FIFO of waiting threads, so a
 * signal wakes exactly one thread that was waiting at that time.
 *
 * Kernel semaphores (runtime_sema.c semantics: counted tokens, FIFO waiters that each need n tokens,
 * cancel/delete wake the waiters with an error) live in the same table: their ids are table indices, so
 * a slot carries ids, counts and waiters to another process or location instance like the rest. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vsync.h"
#pragma comment(lib, "synchronization.lib")

#define E_PERM 1
#define E_DEADLK 11
#define E_NOMEM 12
#define E_BUSY 16
#define E_INVAL 22
#define E_TIMEDOUT 60
static int32_t oerr(int e) { return e ? (int32_t)(0x80020000u | (unsigned)e) : 0; }

typedef struct {
    volatile LONG word;      /* mutex 0 free / 1 locked / 2 locked + waiters; cond: queue spinlock; rwlock: readers or -1 */
    volatile DWORD owner;    /* mutex / rwlock writer: thread id */
    LONG count;              /* mutex recursion depth */
    uint8_t type, kind;      /* VS_*; mutex type 1..4 (attr: the type it sets) */
    uint16_t gen;
    uint32_t seq, next_free; /* creation number; free list */
    uint32_t qhead, qtail;   /* cond: waiting threads (VThr index, 0 = none) */
    uint64_t cell;           /* guest cell of the creation / of the current lock */
    uint8_t dead, pad8[7];   /* destroyed but kept: a slot may still name it (see retire) */
    uint64_t pad[2];
} VObj;                      /* 64 B */
#define VS_CAP (1u << 20)
#define NONE 0xffffffffu
static VObj *g_o;
static volatile LONG g_hw = 1;   /* entries [1, g_hw) were used; 0 is never a handle */
static uint32_t g_free = NONE;
static volatile LONG g_seq;
static SRWLOCK g_alloc = SRWLOCK_INIT;
static volatile LONG g_epoch;      /* lock waiters re-read their cell when it changes */
static volatile LONG g_cepoch;     /* condition waiters return (spurious wakeup) when it changes */
static volatile LONG g_snap_seq;   /* creation number at the latest capture */
static volatile LONG g_quar;       /* destroyed objects kept */
static volatile LONG g_spin = 100;   /* pause iterations before a contended lock sleeps (vs_set_spin) */
static volatile LONG g_live[VS_NTYPES];
static void (*g_log)(const char *, ...);
static int g_on;
/* Lineage of the table: a new id at start and whenever a load replaces the table (another process or
 * location instance). Slots of the current lineage merge (same-process load); others replace. */
static uint64_t g_table_id;
static uint64_t new_table_id(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return ((uint64_t)GetCurrentProcessId() << 40) ^ (uint64_t)t.QuadPart ^ 0x9E3779B97F4A7C15ull * (g_table_id + 1); }
/* Objects each slot saved in this lineage names (index, gen), so destroyed objects nobody needs any more
 * can be freed (quarantine hygiene). Filled at capture (no heap: VirtualAlloc'd at init). */
#define SL_SLOTS 16
#define SL_CAP 65536   /* live objects per slot (about 4500 in play) */
typedef struct { uint64_t table_id; uint32_t n, cap; uint64_t *e; } SlotList;   /* e: gen << 32 | index */
static SlotList g_sl[SL_SLOTS + 1];   /* [SL_SLOTS] = the capture in progress */

typedef struct { uint64_t h, cell; uint32_t count, pad; } RHold;
#define RD_MAX 16
typedef struct DECLSPEC_ALIGN(64) {
    DWORD tid; uint32_t qnext;
    volatile LONG signaled;
    volatile LONG *wait_addr;  /* the address this thread sleeps on (WaitOnAddress), else NULL */
    uint32_t qobj;             /* cond wait: index of the cond whose queue holds this thread */
    uint64_t *wcell;           /* cond wait: the guest cell of that cond */
    volatile LONG wkind;       /* what the thread waits for now: 0 none, 1 mutex, 2 cond, 3 rwlock (vs_thread_wait) */
    uint64_t *wc1, *wc2;       /* its cells (cond: cond, mutex) */
    uint32_t nrd, pad;
    int32_t sneed, sres;       /* semaphore wait: tokens needed, result given by the waker */
    RHold rd[RD_MAX];          /* read holds of rwlocks (EDEADLK/EPERM checks, holds after a load) */
    uint64_t calls[16];
} VThr;
#define VTHR_MAX 8192
static VThr *g_vt;               /* [1, g_nvt] */
static volatile LONG g_nvt;
static __declspec(thread) VThr *t_me;

static VThr *me(void) {
    VThr *t = t_me;
    if (t) return t;
    LONG i = InterlockedIncrement(&g_nvt);
    if (i >= VTHR_MAX) { InterlockedDecrement(&g_nvt); i = VTHR_MAX - 1; if (g_log) g_log("vsync: more than %d threads, sharing the last record", VTHR_MAX); }
    t = &g_vt[i];
    t->tid = GetCurrentThreadId();
    t_me = t;
    return t;
}
static uint32_t vt_index(VThr *t) { return (uint32_t)(t - g_vt); }

static volatile LONG g_nbad;
/* the first refusals are logged with what the cell held */
static int bad(const char *what, uint64_t *cell, uint64_t v) {
    if (g_log && InterlockedIncrement(&g_nbad) <= 40) {
        uint32_t i = (uint32_t)(v & 0xffffff);
        g_log("vsync: %s refused: tid %lu cell %p = 0x%llx (index %u type %d gen %u; table type %d gen %u, high water %ld)", what, GetCurrentThreadId(), cell, v,
              i, (int)((v >> 40) & 0xff), (unsigned)((v >> 24) & 0xffff), i < (uint32_t)g_hw ? g_o[i].type : -1, i < (uint32_t)g_hw ? g_o[i].gen : 0, g_hw);
    }
    return E_INVAL;
}
static int bad_perm(const char *what, uint64_t *cell, DWORD owner, LONG word) {
    if (g_log && InterlockedIncrement(&g_nbad) <= 40) g_log("vsync: %s EPERM: tid %lu cell %p = 0x%llx owner %lu word %ld", what, GetCurrentThreadId(), cell, cell ? *cell : 0, owner, word);
    return E_PERM;
}
static uint64_t mk(uint32_t i, int type, uint16_t gen) { return VS_TAG | ((uint64_t)type << 40) | ((uint64_t)gen << 24) | i; }
static VObj *get(uint64_t h, int type) {
    if ((h & VS_TAGMASK) != VS_TAG || (int)((h >> 40) & 0xff) != type) return NULL;
    uint32_t i = (uint32_t)(h & 0xffffff);
    if (!i || i >= (uint32_t)g_hw) return NULL;
    VObj *o = &g_o[i];
    if (o->type != type || o->gen != (uint16_t)(h >> 24) || o->dead) return NULL;
    return o;
}
int vs_handle_state(uint64_t v) {
    int t;
    if (!vs_is_handle(v, &t)) return 0;
    return get(v, t) ? t : -t;
}
int vs_is_handle(uint64_t v, int *type) {
    if ((v & VS_TAGMASK) != VS_TAG) return 0;
    int t = (int)((v >> 40) & 0xff);
    if (t < 1 || t >= VS_NTYPES) return 0;
    if (type) *type = t;
    return 1;
}
static uint64_t alloc_obj(int type, int kind, uint64_t cell) {
    AcquireSRWLockExclusive(&g_alloc);
    uint32_t i;
    if (g_free != NONE) { i = g_free; g_free = g_o[i].next_free; }
    else if ((uint32_t)g_hw < VS_CAP) i = (uint32_t)g_hw++;
    else { ReleaseSRWLockExclusive(&g_alloc); return 0; }
    VObj *o = &g_o[i];
    uint16_t gen = (uint16_t)(o->gen + 1); if (!gen) gen = 1;
    o->word = 0; o->owner = 0; o->count = 0; o->kind = (uint8_t)kind; o->gen = gen;
    o->seq = (uint32_t)InterlockedIncrement(&g_seq); o->next_free = NONE; o->qhead = o->qtail = 0; o->cell = cell; o->dead = 0;
    o->type = (uint8_t)type;
    ++g_live[type];
    ReleaseSRWLockExclusive(&g_alloc);
    return mk(i, type, gen);
}
static void free_obj(VObj *o);
/* Destroy. An object that existed at the latest capture keeps its index forever (marked dead): a slot
 * can bring it back, and no later object can take its number. Only objects made after the latest
 * capture are recycled, so a slot's objects never collide with the objects of the moment it is loaded. */
static void retire(VObj *o) {
    if ((LONG)o->seq <= g_snap_seq) {
        AcquireSRWLockExclusive(&g_alloc);
        o->dead = 1; --g_live[o->type]; ++g_quar;
        ReleaseSRWLockExclusive(&g_alloc);
    } else free_obj(o);
}
static void free_obj(VObj *o) {
    AcquireSRWLockExclusive(&g_alloc);
    --g_live[o->type];
    o->type = VS_FREE; o->next_free = g_free; g_free = (uint32_t)(o - g_o);
    ReleaseSRWLockExclusive(&g_alloc);
}

/* ---- timeouts ---- */
static uint64_t now_ms(void) { return GetTickCount64(); }
static DWORD left_ms(uint64_t deadline) {   /* deadline 0 = infinite */
    if (!deadline) return INFINITE;
    uint64_t n = now_ms();
    return n >= deadline ? 0 : (DWORD)(deadline - n);
}
/* Deadlines are GetTickCount64 values (never 0 in practice; 0 means no deadline). */
static uint64_t deadline_usec(uint64_t usec) { return now_ms() + (usec + 999) / 1000; }
static int64_t realtime_ms(void) { FILETIME f; GetSystemTimePreciseAsFileTime(&f); return (int64_t)((((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) - 116444736000000000ull) / 10000; }
static uint64_t deadline_abs(int64_t sec, int64_t nsec) {   /* CLOCK_REALTIME, like FreeBSD's default */
    int64_t d = sec * 1000 + (nsec + 999999) / 1000000 - realtime_ms();
    return now_ms() + (d > 0 ? (uint64_t)d : 0);
}
static BOOL sleep_on(VThr *t, volatile LONG *addr, LONG expect, uint64_t deadline) {
    t->wait_addr = addr;
    BOOL r = WaitOnAddress(addr, &expect, sizeof expect, left_ms(deadline));
    t->wait_addr = NULL;
    return r;
}

/* ---- mutex attributes ---- */
static int mattr_init(uint64_t *out) {
    if (!out) return E_INVAL;
    uint64_t h = alloc_obj(VS_MATTR, 1, (uint64_t)out);
    if (!h) return E_NOMEM;
    me()->calls[VC_CREATE]++;
    *out = h; return 0;
}
static int mattr_type(uint64_t *a, int type) {
    VObj *o = a ? get(*a, VS_MATTR) : NULL;
    if (!o || type < 1 || type > 4) return E_INVAL;
    o->kind = (uint8_t)type; return 0;
}
static int mattr_protocol(uint64_t *a, int protocol) {
    if (!a || !get(*a, VS_MATTR) || protocol < 0 || protocol > 2) return E_INVAL;
    return 0;
}
static int mattr_destroy(uint64_t *a) {
    VObj *o = a ? get(*a, VS_MATTR) : NULL;
    if (!o) return E_INVAL;
    retire(o); *a = 0; me()->calls[VC_DESTROY]++; return 0;
}

/* ---- mutexes ---- */
static int mutex_init(uint64_t *out, uint64_t *attr) {
    if (!out || (attr && !*attr)) return E_INVAL;
    int kind = 1;
    if (attr) { VObj *a = get(*attr, VS_MATTR); if (!a) return E_INVAL; kind = a->kind; }
    if (kind < 1 || kind > 4) return E_INVAL;
    uint64_t h = alloc_obj(VS_MUTEX, kind, (uint64_t)out);
    if (!h) return E_NOMEM;
    me()->calls[VC_CREATE]++;
    *out = h; return 0;
}
/* Static initialization (cell 0/1) creates the mutex once, even under contention. */
static int ensure_mutex(uint64_t *cell, VObj **out) {
    if (!cell) return E_INVAL;
    for (;;) {
        uint64_t v = *(volatile uint64_t *)cell;
        if (v == 2) return E_INVAL;
        if (v >= 3) { *out = get(v, VS_MUTEX); return *out ? 0 : bad("mutex", cell, v); }
        uint64_t h = alloc_obj(VS_MUTEX, 1, (uint64_t)cell);
        if (!h) return E_NOMEM;
        if ((uint64_t)InterlockedCompareExchange64((volatile LONG64 *)cell, (LONG64)h, (LONG64)v) == v) { me()->calls[VC_STATIC_INIT]++; *out = get(h, VS_MUTEX); return 0; }
        free_obj(get(h, VS_MUTEX));
    }
}
static void take(VObj *o, DWORD self, uint64_t *cell) { o->owner = self; o->count = 1; o->cell = (uint64_t)cell; }
/* deadline: 0 infinite; try_only: trylock. Re-reads the cell after a load (epoch change). */
static int mutex_acquire(uint64_t *cell, int try_only, uint64_t deadline) {
    VThr *t = me();
    DWORD self = t->tid;
    t->calls[try_only ? VC_TRYLOCK : VC_LOCK]++;
    for (;;) {
        LONG ep = g_epoch;
        VObj *o; int e = ensure_mutex(cell, &o);
        if (e) return e;
        if (o->owner == self && o->word) {
            if (o->kind == 2) { ++o->count; return 0; }
            return try_only ? E_BUSY : E_DEADLK;
        }
        LONG c = InterlockedCompareExchange(&o->word, 1, 0);
        if (!c) { take(o, self, cell); return 0; }
        if (try_only) return E_BUSY;
        t->calls[VC_CONTENDED]++;
        for (LONG i = 0, n = g_spin; i < n; ++i) {
            YieldProcessor();
            if (!o->word && !InterlockedCompareExchange(&o->word, 1, 0)) { take(o, self, cell); return 0; }
        }
        for (;;) {
            if (c == 2 || InterlockedCompareExchange(&o->word, 2, 1) != 0) {
                if (deadline && !left_ms(deadline)) return E_TIMEDOUT;
                if (ep == g_epoch) { t->wc1 = cell; t->wc2 = NULL; t->wkind = 1; sleep_on(t, &o->word, 2, deadline); t->wkind = 0; }
            }
            if (ep != g_epoch) { if (t->wc1) cell = t->wc1; break; }
            c = InterlockedCompareExchange(&o->word, 2, 0);
            if (!c) { take(o, self, cell); return 0; }
        }
        t->calls[VC_RETARGET]++;
    }
}
static int mutex_release(uint64_t *cell) {
    if (!cell) return E_INVAL;
    uint64_t v = *(volatile uint64_t *)cell;
    if (v == 2) return E_INVAL;
    if (v < 2) return E_PERM;
    VObj *o = get(v, VS_MUTEX);
    if (!o) return E_INVAL;
    VThr *t = me();
    t->calls[VC_UNLOCK]++;
    if (!o->word || (o->owner != t->tid && o->kind != 3)) return bad_perm("mutex_unlock", cell, o->owner, o->word);
    if (o->kind == 2 && o->count > 1) { --o->count; return 0; }
    o->owner = 0; o->count = 0;
    if (InterlockedExchange(&o->word, 0) == 2) WakeByAddressSingle((PVOID)&o->word);
    return 0;
}
static int mutex_destroy(uint64_t *cell) {
    if (!cell) return E_INVAL;
    uint64_t v = *cell;
    if (v == 2) return E_INVAL;
    if (v < 2) return 0;
    VObj *o = get(v, VS_MUTEX);
    if (!o) return E_INVAL;
    if (o->word) return E_BUSY;
    retire(o); *cell = 2; me()->calls[VC_DESTROY]++;
    return 0;
}

/* ---- condition variables ---- */
static void qlock(VObj *o) { for (int n = 0; InterlockedCompareExchange(&o->word, 1, 0); ++n) { if (n > 64) SwitchToThread(); else YieldProcessor(); } }
static void qunlock(VObj *o) { InterlockedExchange(&o->word, 0); }
static int cond_init(uint64_t *out) {
    if (!out) return E_INVAL;
    uint64_t h = alloc_obj(VS_COND, 0, (uint64_t)out);
    if (!h) return E_NOMEM;
    me()->calls[VC_CREATE]++;
    *out = h; return 0;
}
static int ensure_cond(uint64_t *cell, VObj **out) {
    if (!cell) return E_INVAL;
    for (;;) {
        uint64_t v = *(volatile uint64_t *)cell;
        if (v >= 2) { *out = get(v, VS_COND); return *out ? 0 : bad("cond", cell, v); }
        uint64_t h = alloc_obj(VS_COND, 0, (uint64_t)cell);
        if (!h) return E_NOMEM;
        if ((uint64_t)InterlockedCompareExchange64((volatile LONG64 *)cell, (LONG64)h, (LONG64)v) == v) { me()->calls[VC_STATIC_INIT]++; *out = get(h, VS_COND); return 0; }
        free_obj(get(h, VS_COND));
    }
}
static int cond_destroy(uint64_t *cell) {
    if (!cell) return E_INVAL;
    if (*cell < 2) return 0;
    VObj *o = get(*cell, VS_COND);
    if (!o) return E_INVAL;
    if (o->qhead) return E_BUSY;
    retire(o); *cell = 0; me()->calls[VC_DESTROY]++;
    return 0;
}
static int cond_wait(uint64_t *ccell, uint64_t *mcell, uint64_t deadline) {
    VObj *c; int e = ensure_cond(ccell, &c);
    if (e) return e;
    if (!mcell || *mcell < 3) return bad("cond_wait mutex", mcell, mcell ? *mcell : 0);
    VObj *m = get(*mcell, VS_MUTEX);
    if (!m) return bad("cond_wait mutex", mcell, *mcell);
    VThr *t = me();
    if (m->owner != t->tid || !m->word) return bad_perm("cond_wait", mcell, m->owner, m->word);
    t->calls[VC_CWAIT]++;
    uint32_t me_i = vt_index(t);
    LONG ep = g_cepoch;
    qlock(c);
    t->signaled = 0; t->qnext = 0; t->qobj = (uint32_t)(c - g_o); t->wcell = ccell;
    if (c->qtail) g_vt[c->qtail].qnext = me_i; else c->qhead = me_i;
    c->qtail = me_i;
    qunlock(c);
    LONG depth = m->count;
    m->count = 1;
    mutex_release(mcell);
    int timed_out = 0;
    t->wc1 = ccell; t->wc2 = mcell; t->wkind = 2;
    while (!t->signaled && ep == g_cepoch) {
        if (!sleep_on(t, &t->signaled, 0, deadline) && GetLastError() == ERROR_TIMEOUT) { timed_out = 1; break; }
    }
    if (!t->signaled && ep == g_cepoch) {          /* timeout: leave the queue unless a signal took us out */
        int found = 0;
        c = &g_o[t->qobj];                           /* a load may have moved this thread to another queue */
        qlock(c);
        for (uint32_t *p = &c->qhead, prev = 0; *p; prev = *p, p = &g_vt[*p].qnext)
            if (*p == me_i) { *p = t->qnext; if (c->qtail == me_i) c->qtail = prev; found = 1; break; }
        qunlock(c);
        if (!found) { while (!t->signaled && ep == g_cepoch) YieldProcessor(); timed_out = 0; }
    }
    if (t->wc2) mcell = t->wc2;   /* a cross-process load may have given this thread another mutex cell */
    t->qobj = 0; t->wcell = NULL; t->wkind = 0;
    e = mutex_acquire(mcell, 0, 0);
    if (!e && depth > 1) { VObj *m2 = get(*mcell, VS_MUTEX); if (m2) m2->count = depth; }
    return e ? e : timed_out ? E_TIMEDOUT : 0;
}
static int cond_wake(uint64_t *ccell, int all) {
    VObj *c; int e = ensure_cond(ccell, &c);
    if (e) return e;
    me()->calls[VC_CSIGNAL]++;
    qlock(c);
    uint32_t i = c->qhead;
    if (i) {
        if (all) c->qhead = c->qtail = 0;
        else { c->qhead = g_vt[i].qnext; if (!c->qhead) c->qtail = 0; g_vt[i].qnext = 0; }
    }
    qunlock(c);
    while (i) {
        uint32_t next = all ? g_vt[i].qnext : 0;
        g_vt[i].signaled = 1;
        WakeByAddressSingle((PVOID)&g_vt[i].signaled);
        i = next;
    }
    return 0;
}

/* ---- rwlocks ---- */
static RHold *rd_find(VThr *t, uint64_t h) { for (uint32_t i = 0; i < t->nrd; ++i) if (t->rd[i].h == h) return &t->rd[i]; return NULL; }
static int rw_init(uint64_t *out, uint64_t *attr) {
    if (!out || (attr && !*attr)) return E_INVAL;
    uint64_t h = alloc_obj(VS_RWLOCK, 0, (uint64_t)out);
    if (!h) return E_NOMEM;
    me()->calls[VC_CREATE]++;
    *out = h; return 0;
}
static int ensure_rw(uint64_t *cell, VObj **out, uint64_t *hv) {
    if (!cell) return E_INVAL;
    for (;;) {
        uint64_t v = *(volatile uint64_t *)cell;
        if (v) { *out = get(v, VS_RWLOCK); *hv = v; return *out ? 0 : bad("rwlock", cell, v); }
        uint64_t h = alloc_obj(VS_RWLOCK, 0, (uint64_t)cell);
        if (!h) return E_NOMEM;
        if (InterlockedCompareExchange64((volatile LONG64 *)cell, (LONG64)h, 0) == 0) { me()->calls[VC_STATIC_INIT]++; *out = get(h, VS_RWLOCK); *hv = h; return 0; }
        free_obj(get(h, VS_RWLOCK));
    }
}
/* writer: 1 for write; mode: 0 blocking, 1 try, 2 timed (deadline) */
static int rw_acquire(uint64_t *cell, int writer, int mode, uint64_t deadline) {
    VThr *t = me();
    t->calls[writer ? VC_RWWRITE : VC_RWREAD]++;
    for (;;) {
        LONG ep = g_epoch;
        VObj *o; uint64_t h; int e = ensure_rw(cell, &o, &h);
        if (e) return e;
        RHold *r = rd_find(t, h);
        if ((o->word == -1 && o->owner == t->tid) || (writer && r)) return mode == 1 ? E_BUSY : E_DEADLK;
        if (!writer && !r && t->nrd == RD_MAX) return 35; /* EAGAIN */
        for (;;) {
            LONG s = o->word;
            if (writer ? s == 0 : s >= 0) {
                if (InterlockedCompareExchange(&o->word, writer ? -1 : s + 1, s) != s) continue;
                if (writer) { o->owner = t->tid; o->cell = (uint64_t)cell; }
                else if (r) ++r->count;
                else { t->rd[t->nrd].h = h; t->rd[t->nrd].cell = (uint64_t)cell; t->rd[t->nrd].count = 1; ++t->nrd; }
                return 0;
            }
            if (mode == 1) return E_BUSY;
            if (mode == 2 && !left_ms(deadline)) return E_TIMEDOUT;
            int freed = 0;
            for (LONG i = 0, n = g_spin; i < n && !freed; ++i) { YieldProcessor(); LONG w = o->word; freed = writer ? w == 0 : w >= 0; }
            if (freed) continue;
            if (ep == g_epoch) { t->wc1 = cell; t->wc2 = NULL; t->wkind = 3; sleep_on(t, &o->word, s, mode == 2 ? deadline : 0); t->wkind = 0; }
            if (ep != g_epoch) { if (t->wc1) cell = t->wc1; break; }
        }
        t->calls[VC_RETARGET]++;
    }
}
static int rw_unlock(uint64_t *cell) {
    if (!cell) return E_INVAL;
    uint64_t h = *cell;
    VObj *o = get(h, VS_RWLOCK);
    if (!o) return E_INVAL;
    VThr *t = me();
    t->calls[VC_RWUNLOCK]++;
    if (o->word == -1) {
        if (o->owner != t->tid) return E_PERM;
        o->owner = 0;
        InterlockedExchange(&o->word, 0);
        WakeByAddressAll((PVOID)&o->word);
        return 0;
    }
    RHold *r = rd_find(t, h);
    if (!r || o->word <= 0) return E_PERM;
    if (!--r->count) *r = t->rd[--t->nrd];
    if (InterlockedDecrement(&o->word) == 0) WakeByAddressAll((PVOID)&o->word);
    return 0;
}
static int rw_destroy(uint64_t *cell) {
    if (!cell) return E_INVAL;
    if (!*cell) return 0;
    VObj *o = get(*cell, VS_RWLOCK);
    if (!o) return E_INVAL;
    if (o->word) return E_BUSY;
    retire(o); *cell = 1; me()->calls[VC_DESTROY]++;
    return 0;
}

/* ---- kernel semaphores ----
 * VObj: word = count, count = maximum, kind = attr, pad[0] = initial, owner = spinlock of the waiter
 * queue (qhead/qtail: VThr indices, FIFO; VThr.sneed tokens each). */
static void slock(VObj *o) { for (int n = 0; InterlockedCompareExchange((volatile LONG *)&o->owner, 1, 0); ++n) { if (n > 64) SwitchToThread(); else YieldProcessor(); } }
static void sunlock(VObj *o) { InterlockedExchange((volatile LONG *)&o->owner, 0); }
static uint32_t sema_id(uint32_t i, uint16_t gen) { return VS_SEMA_TAG | ((uint32_t)(gen & 0x3f) << 24) | i; }
static VObj *sema_get(uint32_t id) {
    if ((id & 0xC0000000u) != VS_SEMA_TAG) return NULL;
    uint32_t i = id & 0xffffff;
    if (!i || i >= (uint32_t)g_hw) return NULL;
    VObj *o = &g_o[i];
    if (o->type != VS_SEMA || (o->gen & 0x3f) != ((id >> 24) & 0x3f) || o->dead) return NULL;
    return o;
}
/* Gives tokens to waiters in FIFO order (one that needs more than there is gets skipped, as in the
 * runtime); queue lock held. The woken are listed for wake_list after the unlock. */
static uint32_t sema_serve(VObj *o, uint32_t *woken, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t *p = &o->qhead, prev = 0; *p;) {
        VThr *w = &g_vt[*p];
        if (w->sneed > o->word || n == cap) { prev = *p; p = &w->qnext; continue; }
        uint32_t i = *p;
        o->word -= w->sneed; w->sres = 0;
        woken[n++] = i;
        if (o->qtail == i) o->qtail = prev;
        *p = w->qnext; w->qnext = 0;
    }
    return n;
}
static void wake_list(const uint32_t *w, uint32_t n) { for (uint32_t k = 0; k < n; ++k) { g_vt[w[k]].signaled = 1; WakeByAddressSingle((PVOID)&g_vt[w[k]].signaled); } }
static uint32_t sema_wake_all(VObj *o, int32_t result, uint32_t *woken, uint32_t cap) {
    uint32_t n = 0;
    while (o->qhead && n < cap) { uint32_t i = o->qhead; VThr *w = &g_vt[i]; o->qhead = w->qnext; w->qnext = 0; w->sres = result; woken[n++] = i; }
    if (!o->qhead) o->qtail = 0;
    return n;
}
#define SWAKE_CAP 512
static int32_t sema_create(uint32_t *out, const char *name, uint32_t attr, int32_t initial, int32_t maximum) {
    if (!out || !name || attr > 2 || initial < 0 || maximum <= 0 || initial > maximum) return oerr(E_INVAL);
    uint64_t h = alloc_obj(VS_SEMA, (int)attr, (uint64_t)out);
    if (!h) return oerr(E_NOMEM);
    VObj *o = &g_o[h & 0xffffff];
    o->word = initial; o->count = maximum; o->pad[0] = (uint64_t)(uint32_t)initial;
    me()->calls[VC_CREATE]++;
    *out = sema_id((uint32_t)(h & 0xffffff), o->gen);
    return 0;
}
static int32_t sema_wait_count(uint32_t id, int32_t need, uint32_t *timeout, int block) {
    VObj *o = sema_get(id);
    if (!o) return oerr(3);
    if (need <= 0 || need > o->count) return oerr(E_INVAL);
    VThr *t = me();
    t->calls[VC_SWAIT]++;
    uint32_t w[SWAKE_CAP], nw = 0;
    slock(o);
    if (o->qhead && o->word > 0) nw = sema_serve(o, w, SWAKE_CAP);   /* earlier waiters first (FIFO) */
    if (o->word >= need) { o->word -= need; sunlock(o); wake_list(w, nw); return 0; }
    if (!block) { sunlock(o); wake_list(w, nw); return oerr(E_BUSY); }
    if (timeout && !*timeout) { sunlock(o); wake_list(w, nw); return oerr(E_TIMEDOUT); }
    uint32_t me_i = vt_index(t);
    t->signaled = 0; t->qnext = 0; t->sneed = need; t->sres = 0; t->qobj = (uint32_t)(o - g_o);
    if (o->qtail) g_vt[o->qtail].qnext = me_i; else o->qhead = me_i;
    o->qtail = me_i;
    sunlock(o);
    wake_list(w, nw);
    uint64_t t0 = GetTickCount64(), deadline = timeout ? t0 + (*timeout + 999) / 1000 : 0;
    t->wc1 = (uint64_t *)(uint64_t)id; t->wc2 = (uint64_t *)(uint64_t)need; t->wkind = 4;
    for (;;) {
        while (!t->signaled) {
            LONG ep = g_cepoch;
            if (!sleep_on(t, &t->signaled, 0, deadline) && GetLastError() == ERROR_TIMEOUT && ep == g_cepoch) break;
        }
        if (t->signaled) break;
        o = &g_o[t->qobj];   /* timeout: leave the queue (a load may have moved us to another) unless a waker took us out */
        slock(o);
        int found = 0;
        for (uint32_t *p = &o->qhead, prev = 0; *p; prev = *p, p = &g_vt[*p].qnext)
            if (*p == me_i) { *p = t->qnext; if (o->qtail == me_i) o->qtail = prev; found = 1; break; }
        sunlock(o);
        if (found) { t->sres = oerr(E_TIMEDOUT); break; }
        while (!t->signaled) YieldProcessor();
        break;
    }
    t->wkind = 0; t->qobj = 0;
    int32_t r = t->sres;
    if (timeout) { uint64_t el = (GetTickCount64() - t0) * 1000; *timeout = r || el >= *timeout ? 0 : (uint32_t)(*timeout - el); }
    return r;
}
static int32_t sema_signal(uint32_t id, int32_t count) {
    VObj *o = sema_get(id);
    if (!o) return oerr(3);
    me()->calls[VC_SSIGNAL]++;
    slock(o);
    if (count <= 0 || count > o->count - o->word) { sunlock(o); return oerr(E_INVAL); }
    o->word += count;
    uint32_t w[SWAKE_CAP]; uint32_t n = sema_serve(o, w, SWAKE_CAP);
    sunlock(o);
    wake_list(w, n);
    return 0;
}
static int32_t sema_cancel(uint32_t id, int32_t count, int32_t *waiters) {
    VObj *o = sema_get(id);
    if (!o) return oerr(3);
    if (count > o->count) return oerr(E_INVAL);
    slock(o);
    uint32_t w[SWAKE_CAP]; uint32_t n = sema_wake_all(o, oerr(85), w, SWAKE_CAP);
    o->word = count < 0 ? (LONG)o->pad[0] : count;
    sunlock(o);
    wake_list(w, n);
    if (waiters) *waiters = (int32_t)n;
    return 0;
}
static int32_t sema_delete(uint32_t id) {
    VObj *o = sema_get(id);
    if (!o) return oerr(3);
    slock(o);
    uint32_t w[SWAKE_CAP]; uint32_t n = sema_wake_all(o, oerr(13), w, SWAKE_CAP);
    sunlock(o);
    wake_list(w, n);
    retire(o); me()->calls[VC_DESTROY]++;
    return 0;
}
/* After a load (threads suspended): tokens of the restored counts go to the waiters there are now. */
static uint32_t sema_kick_all(void) {
    uint32_t total = 0;
    for (uint32_t i = 1; i < (uint32_t)g_hw; ++i) {
        VObj *o = &g_o[i];
        if (o->type != VS_SEMA || o->dead || !o->qhead || o->word <= 0) continue;
        uint32_t w[SWAKE_CAP]; uint32_t n = sema_serve(o, w, SWAKE_CAP);
        wake_list(w, n); total += n;
    }
    return total;
}

/* ---- guest entry points (Win64; thunks.asm adapts the System V calls) ---- */
typedef struct { int64_t sec, nsec; } GTime;
static int32_t S(int e) { if (e && e != E_BUSY && e != E_TIMEDOUT) me()->calls[VC_ERRORS]++; return oerr(e); }
static int32_t P(int e) { if (e && e != E_BUSY && e != E_TIMEDOUT) me()->calls[VC_ERRORS]++; return e; }
static int valid_time(const GTime *t) { return t && t->nsec >= 0 && t->nsec < 1000000000; }
static int cond_abs(uint64_t *c, uint64_t *m, const GTime *t) { return valid_time(t) ? cond_wait(c, m, deadline_abs(t->sec, t->nsec)) : E_INVAL; }
static int rw_timed(uint64_t *r, int writer, const GTime *t) {
    int e = rw_acquire(r, writer, 1, 0);
    if (e != E_BUSY) return e;
    if (!valid_time(t)) return E_INVAL;
    return rw_acquire(r, writer, 2, deadline_abs(t->sec, t->nsec));
}
/* scePthread* (Orbis errors) */
int32_t vs_mattr_init_c(uint64_t *a) { return S(mattr_init(a)); }
int32_t vs_mattr_type_c(uint64_t *a, int t) { return S(mattr_type(a, t)); }
int32_t vs_mattr_destroy_c(uint64_t *a) { return S(mattr_destroy(a)); }
int32_t vs_mattr_protocol_c(uint64_t *a, int p) { return S(mattr_protocol(a, p)); }
int32_t vs_mutex_init_c(uint64_t *o, uint64_t *a, const char *n) { (void)n; return S(mutex_init(o, a)); }
int32_t vs_mutex_lock_c(uint64_t *m) { return S(mutex_acquire(m, 0, 0)); }
int32_t vs_mutex_trylock_c(uint64_t *m) { return S(mutex_acquire(m, 1, 0)); }
int32_t vs_mutex_timedlock_c(uint64_t *m, uint32_t usec) { return S(mutex_acquire(m, 0, deadline_usec(usec))); }
int32_t vs_mutex_unlock_c(uint64_t *m) { return S(mutex_release(m)); }
int32_t vs_mutex_destroy_c(uint64_t *m) { return S(mutex_destroy(m)); }
int32_t vs_cond_init_c(uint64_t *c, void *a, const char *n) { (void)a; (void)n; return S(cond_init(c)); }
int32_t vs_cond_destroy_c(uint64_t *c) { return S(cond_destroy(c)); }
int32_t vs_cond_wait_c(uint64_t *c, uint64_t *m) { return S(cond_wait(c, m, 0)); }
int32_t vs_cond_timedwait_c(uint64_t *c, uint64_t *m, uint32_t usec) { return S(cond_wait(c, m, deadline_usec(usec))); }
int32_t vs_cond_signal_c(uint64_t *c) { return S(cond_wake(c, 0)); }
int32_t vs_cond_broadcast_c(uint64_t *c) { return S(cond_wake(c, 1)); }
int32_t vs_rw_init_c(uint64_t *o, uint64_t *a, const char *n) { (void)n; return S(rw_init(o, a)); }
int32_t vs_rw_destroy_c(uint64_t *r) { return S(rw_destroy(r)); }
int32_t vs_rw_read_c(uint64_t *r) { return S(rw_acquire(r, 0, 0, 0)); }
int32_t vs_rw_write_c(uint64_t *r) { return S(rw_acquire(r, 1, 0, 0)); }
int32_t vs_rw_tryread_c(uint64_t *r) { return S(rw_acquire(r, 0, 1, 0)); }
int32_t vs_rw_trywrite_c(uint64_t *r) { return S(rw_acquire(r, 1, 1, 0)); }
int32_t vs_rw_unlock_c(uint64_t *r) { return S(rw_unlock(r)); }
int32_t vs_rw_timedread_c(uint64_t *r, const GTime *t) { return S(rw_timed(r, 0, t)); }
int32_t vs_rw_timedwrite_c(uint64_t *r, const GTime *t) { return S(rw_timed(r, 1, t)); }
/* pthread_* (positive errno) */
int32_t vs_pattr_init_c(uint64_t *a) { return P(mattr_init(a)); }
int32_t vs_pattr_type_c(uint64_t *a, int t) { return P(mattr_type(a, t)); }
int32_t vs_pattr_destroy_c(uint64_t *a) { return P(mattr_destroy(a)); }
int32_t vs_pmutex_init_c(uint64_t *o, uint64_t *a) { return P(mutex_init(o, a)); }
int32_t vs_pmutex_destroy_c(uint64_t *m) { return P(mutex_destroy(m)); }
int32_t vs_pmutex_lock_c(uint64_t *m) { return P(mutex_acquire(m, 0, 0)); }
int32_t vs_pmutex_trylock_c(uint64_t *m) { return P(mutex_acquire(m, 1, 0)); }
int32_t vs_pmutex_unlock_c(uint64_t *m) { return P(mutex_release(m)); }
int32_t vs_pcond_init_c(uint64_t *c, void *a) { (void)a; return P(cond_init(c)); }
int32_t vs_pcond_destroy_c(uint64_t *c) { return P(cond_destroy(c)); }
int32_t vs_pcond_wait_c(uint64_t *c, uint64_t *m) { return P(cond_wait(c, m, 0)); }
int32_t vs_pcond_timedwait_c(uint64_t *c, uint64_t *m, const GTime *t) { return P(cond_abs(c, m, t)); }
int32_t vs_pcond_signal_c(uint64_t *c) { return P(cond_wake(c, 0)); }
int32_t vs_pcond_broadcast_c(uint64_t *c) { return P(cond_wake(c, 1)); }
/* sceKernel*Sema (Orbis errors); create has 6 arguments: the thunk passes the 4th-6th as a block */
typedef struct { int64_t initial, maximum; const void *options; } SemaArgs;
int32_t vs_sema_create_c(uint32_t *out, const char *name, uint32_t attr, const SemaArgs *a) { return sema_create(out, name, attr, (int32_t)a->initial, (int32_t)a->maximum); }
int32_t vs_sema_wait_c(uint32_t id, int32_t need, uint32_t *timeout) { return sema_wait_count(id, need, timeout, 1); }
int32_t vs_sema_poll_c(uint32_t id, int32_t need) { return sema_wait_count(id, need, NULL, 0); }
int32_t vs_sema_signal_c(uint32_t id, int32_t count) { return sema_signal(id, count); }
int32_t vs_sema_cancel_c(uint32_t id, int32_t count, int32_t *waiters) { return sema_cancel(id, count, waiters); }
int32_t vs_sema_delete_c(uint32_t id) { return sema_delete(id); }

#define T(n) extern void n(void);
T(vs_mattr_init) T(vs_mattr_type) T(vs_mattr_destroy) T(vs_mattr_protocol) T(vs_mutex_init) T(vs_mutex_lock) T(vs_mutex_trylock)
T(vs_mutex_timedlock) T(vs_mutex_unlock) T(vs_mutex_destroy) T(vs_cond_init) T(vs_cond_destroy) T(vs_cond_wait) T(vs_cond_timedwait)
T(vs_cond_signal) T(vs_cond_broadcast) T(vs_rw_init) T(vs_rw_destroy) T(vs_rw_read) T(vs_rw_write) T(vs_rw_tryread) T(vs_rw_trywrite)
T(vs_rw_unlock) T(vs_rw_timedread) T(vs_rw_timedwrite) T(vs_pattr_init) T(vs_pattr_type) T(vs_pattr_destroy) T(vs_pmutex_init)
T(vs_pmutex_destroy) T(vs_pmutex_lock) T(vs_pmutex_trylock) T(vs_pmutex_unlock) T(vs_pcond_init) T(vs_pcond_destroy) T(vs_pcond_wait)
T(vs_pcond_timedwait) T(vs_pcond_signal) T(vs_pcond_broadcast)
T(vs_sema_wait) T(vs_sema_poll) T(vs_sema_signal) T(vs_sema_cancel) T(vs_sema_delete)
extern void vs_sema_create(void);
#undef T
/* NIDs as runtime_mutex.c / runtime_rwlock.c resolve them */
static const struct { const char *nid; void (*fn)(void); } k_nids[] = {
    {"F8bUHwAG284#p#J", vs_mattr_init}, {"iMp8QpE+XO4#p#J", vs_mattr_type}, {"smWEktiyyG0#p#J", vs_mattr_destroy},
    {"1FGvU0i9saQ#p#J", vs_mattr_protocol}, {"cmo1RIYva9o#p#J", vs_mutex_init}, {"9UK1vLZQft4#p#J", vs_mutex_lock},
    {"upoVrzMHFeE#p#J", vs_mutex_trylock}, {"IafI2PxcPnQ#p#J", vs_mutex_timedlock}, {"tn3VlD0hG60#p#J", vs_mutex_unlock},
    {"2Of0f+3mhhE#p#J", vs_mutex_destroy}, {"2Tb92quprl0#p#J", vs_cond_init}, {"g+PZd2hiacg#p#J", vs_cond_destroy},
    {"WKAXJ4XBPQ4#p#J", vs_cond_wait}, {"BmMjYxmew1w#p#J", vs_cond_timedwait}, {"kDh-NfxgMtE#p#J", vs_cond_signal},
    {"JGgj7Uvrl+A#p#J", vs_cond_broadcast}, {"6ULAa0fq4jA#p#J", vs_rw_init}, {"BB+kb08Tl9A#p#J", vs_rw_destroy},
    {"Ox9i0c7L5w0#p#J", vs_rw_read}, {"mqdNorrB+gI#p#J", vs_rw_write}, {"XD3mDeybCnk#p#J", vs_rw_tryread},
    {"bIHoZCTomsI#p#J", vs_rw_trywrite}, {"+L98PIbGttk#p#J", vs_rw_unlock}, {"iPtZRWICjrM#p#J", vs_rw_timedread},
    {"adh--6nIqTk#p#J", vs_rw_timedwrite},
    {"dQHWEsJtoE4#I#J", vs_pattr_init}, {"mDmgMOGVUqg#I#J", vs_pattr_type}, {"HF7lK46xzjY#I#J", vs_pattr_destroy},
    {"ttHNfU+qDBU#I#J", vs_pmutex_init}, {"ltCfaGr2JGE#I#J", vs_pmutex_destroy}, {"7H0iTOciTLo#I#J", vs_pmutex_lock},
    {"K-jXhbt2gn4#I#J", vs_pmutex_trylock}, {"2Z+PpY6CaJg#I#J", vs_pmutex_unlock}, {"0TyVk4MSLt0#I#J", vs_pcond_init},
    {"RXXqi4CtF8w#I#J", vs_pcond_destroy}, {"Op8TBGY5KHg#I#J", vs_pcond_wait}, {"27bAgiJmOh0#I#J", vs_pcond_timedwait},
    {"2MOy+rUfuhQ#I#J", vs_pcond_signal}, {"mkx2fVhNMsg#I#J", vs_pcond_broadcast},
    /* kernel semaphores (runtime_sema.c). New entries only at the end: the gate keeps this order */
    {"188x57JYp0g#p#J", vs_sema_create}, {"Zxa0VhQVTsk#p#J", vs_sema_wait}, {"4czppHBiriw#p#J", vs_sema_signal},
    {"12wOHk8ywb0#p#J", vs_sema_poll}, {"4DM06U2BNEY#p#J", vs_sema_cancel}, {"R1Jvn8bSCW8#p#J", vs_sema_delete},
};
int vs_entries(void (**out)(void), int cap) {   /* the wrappers in k_nids order (the gate's layout) */
    int n = 0;
    for (size_t i = 0; i < sizeof k_nids / sizeof *k_nids && n < cap; ++i) out[n++] = k_nids[i].fn;
    return n;
}
static volatile LONG g_resolved;
uint64_t vs_lookup(const char *nid) {
    if (!g_on || !nid) return 0;
    for (size_t i = 0; i < sizeof k_nids / sizeof *k_nids; ++i)
        if (!strcmp(nid, k_nids[i].nid)) { InterlockedIncrement(&g_resolved); return (uint64_t)k_nids[i].fn; }
    return 0;
}

int vs_init(void (*log)(const char *, ...)) {
    g_log = log;
    g_o = (VObj *)VirtualAlloc(NULL, (SIZE_T)VS_CAP * sizeof(VObj), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_vt = (VThr *)VirtualAlloc(NULL, (SIZE_T)VTHR_MAX * sizeof(VThr), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    for (int s = 0; s <= SL_SLOTS; ++s) { g_sl[s].cap = SL_CAP; g_sl[s].e = (uint64_t *)VirtualAlloc(NULL, (SIZE_T)SL_CAP * 8, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }
    g_table_id = new_table_id();
    g_on = g_o && g_vt;
    return g_on;
}
int vs_enabled(void) { return g_on && g_resolved > 0; }
void vs_counts(VsCounts *out) {
    memset(out, 0, sizeof *out);
    LONG n = g_nvt < VTHR_MAX ? g_nvt : VTHR_MAX - 1;
    for (LONG i = 1; i <= n; ++i) for (int k = 0; k < 16; ++k) out->calls[k] += g_vt[i].calls[k];
}
int vs_thread_wait(unsigned long tid, uint64_t *c1, uint64_t *c2) {
    LONG n = g_nvt < VTHR_MAX ? g_nvt : VTHR_MAX - 1;
    for (LONG i = n; i >= 1; --i)   /* newest record first: thread ids are reused */
        if (g_vt[i].tid == tid) { *c1 = (uint64_t)g_vt[i].wc1; *c2 = (uint64_t)g_vt[i].wc2; return g_vt[i].wkind; }
    *c1 = *c2 = 0; return 0;
}
void vs_set_spin(int n) { g_spin = n < 0 ? 0 : n; }
uint32_t vs_live(int type) { return type > 0 && type < VS_NTYPES ? (uint32_t)g_live[type] : 0; }
uint32_t vs_high_water(void) { return (uint32_t)g_hw; }

/* ---- slots ---- */
#define VS_TABLE_VERSION 2   /* 2: semaphores */
typedef struct { uint32_t magic, version, hw, free_head; uint32_t seq, nholds, live[VS_NTYPES], pad; uint64_t table_id; } VsHdr;

typedef struct { uint64_t h, cell; uint32_t tid, count, type, pad; } VsHold;
enum { HOLD_MUTEX = 1, HOLD_WRITE = 2, HOLD_READ = 3 };
static uint32_t collect_holds(VsHold *out, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t i = 1; i < (uint32_t)g_hw && n < cap; ++i) {
        VObj *o = &g_o[i];
        if (o->type == VS_MUTEX && o->word && o->owner) { out[n].h = mk(i, VS_MUTEX, o->gen); out[n].cell = o->cell; out[n].tid = o->owner; out[n].count = (uint32_t)o->count; out[n].type = HOLD_MUTEX; ++n; }
        else if (o->type == VS_RWLOCK && o->word == -1) { out[n].h = mk(i, VS_RWLOCK, o->gen); out[n].cell = o->cell; out[n].tid = o->owner; out[n].count = 1; out[n].type = HOLD_WRITE; ++n; }
    }
    LONG nt = g_nvt < VTHR_MAX ? g_nvt : VTHR_MAX - 1;
    for (LONG i = 1; i <= nt; ++i)
        for (uint32_t k = 0; k < g_vt[i].nrd && n < cap; ++k) { out[n].h = g_vt[i].rd[k].h; out[n].cell = g_vt[i].rd[k].cell; out[n].tid = g_vt[i].tid; out[n].count = g_vt[i].rd[k].count; out[n].type = HOLD_READ; ++n; }
    return n;
}
#define HOLD_CAP 4096
uint64_t vs_capture(uint8_t *b, uint64_t cap) {
    static VsHold holds[HOLD_CAP];
    uint32_t nh = collect_holds(holds, HOLD_CAP), hw = (uint32_t)g_hw;
    uint64_t len = sizeof(VsHdr) + (uint64_t)hw * sizeof(VObj) + nh * sizeof(VsHold);
    if (len > cap) return 0;
    g_snap_seq = g_seq;
    VsHdr h = {0x434E5953u, VS_TABLE_VERSION, hw, g_free, (uint32_t)g_seq, nh, {0}, 0, g_table_id};
    SlotList *pl = &g_sl[SL_SLOTS]; pl->n = 0; pl->table_id = g_table_id;
    for (uint32_t i = 1; i < hw && pl->e; ++i) if (g_o[i].type != VS_FREE && !g_o[i].dead && pl->n < pl->cap) pl->e[pl->n++] = (uint64_t)g_o[i].gen << 32 | i;
    for (int i = 0; i < VS_NTYPES; ++i) h.live[i] = (uint32_t)g_live[i];
    memcpy(b, &h, sizeof h);
    memcpy(b + sizeof h, g_o, (size_t)hw * sizeof(VObj));
    memcpy(b + sizeof h + (size_t)hw * sizeof(VObj), holds, nh * sizeof(VsHold));
    return len;
}
static VObj *g_cur;   /* scratch: the table before a restore */
static void rebuild_free(void);
uint64_t vs_table_id(void) { return g_table_id; }
uint64_t vs_slot_table_id(const uint8_t *b, uint64_t len) {
    const VsHdr *h = (const VsHdr *)b;
    return b && len >= sizeof *h && h->magic == 0x434E5953u && h->version == VS_TABLE_VERSION ? h->table_id : 0;
}
void vs_slot_commit(int slot, int ok) {   /* the capture in progress belongs to slot (1..) now */
    if (slot < 1 || slot >= SL_SLOTS) return;
    SlotList *pl = &g_sl[SL_SLOTS], *d = &g_sl[slot];
    if (!ok || !pl->e || !d->e) return;
    memcpy(d->e, pl->e, (size_t)pl->n * 8); d->n = pl->n; d->table_id = pl->table_id;
}
void vs_slot_forget(int slot) { if (slot >= 1 && slot < SL_SLOTS) g_sl[slot].n = 0; }
/* Frees destroyed objects that no slot of this lineage names; reserves (as destroyed) indices such a slot
 * names that are free now. Returns the number freed. */
uint32_t vs_hygiene(uint32_t *kept_out) {
    static uint8_t mark[VS_CAP / 8];
    memset(mark, 0, sizeof mark);
    AcquireSRWLockExclusive(&g_alloc);
    uint32_t freed = 0, reserved = 0;
    for (int s = 1; s < SL_SLOTS; ++s) {
        SlotList *l = &g_sl[s];
        if (!l->n || l->table_id != g_table_id) continue;
        for (uint32_t k = 0; k < l->n; ++k) {
            uint32_t i = (uint32_t)l->e[k]; uint16_t gen = (uint16_t)(l->e[k] >> 32);
            if (i >= (uint32_t)g_hw) continue;
            VObj *o = &g_o[i];
            if (o->type != VS_FREE && o->gen == gen) mark[i >> 3] |= (uint8_t)(1 << (i & 7));
        }
    }
    for (uint32_t i = 1; i < (uint32_t)g_hw; ++i) {
        VObj *o = &g_o[i];
        if (o->type != VS_FREE && o->dead && !(mark[i >> 3] & (1 << (i & 7)))) { o->type = VS_FREE; o->dead = 0; ++freed; }
    }
    (void)reserved;
    rebuild_free();
    if (kept_out) *kept_out = (uint32_t)g_quar;
    ReleaseSRWLockExclusive(&g_alloc);
    return freed;
}
static int find_ref(const uint64_t *refs, uint32_t n, uint64_t v) { for (uint32_t i = 0; i < n; ++i) if (refs[i] == v) return 1; return 0; }
static int obj_live(const VObj *o) { return o->type != VS_FREE && !o->dead; }
/* An object brought back from a slot: no holder, no waiters; a semaphore keeps its count and maximum. */
static void fresh_state(VObj *o) {
    o->owner = 0; o->qhead = o->qtail = 0;
    if (o->type != VS_SEMA) { o->word = 0; o->count = 0; }
}
static void rebuild_free(void) {
    g_free = NONE;
    for (int t = 0; t < VS_NTYPES; ++t) g_live[t] = 0;
    g_quar = 0;
    for (uint32_t i = (uint32_t)g_hw - 1; i >= 1; --i) {
        if (g_o[i].type == VS_FREE) { g_o[i].next_free = g_free; g_free = i; }
        else if (g_o[i].dead) ++g_quar;
        else ++g_live[g_o[i].type];
    }
}
/* Same process: the slot's objects come back at their own indices (nothing took them, see retire),
 * objects of now stay as they are (locks, waiting threads): nobody is woken, as with the host objects
 * the runtime keeps alive without vsync. */
static void restore_merge(const VsHdr *h, const VObj *snap, char *note, size_t nn) {
    uint32_t revived = 0, kept_now = 0, conflicts = 0, sema_counts = 0, hw = h->hw > (uint32_t)g_hw ? h->hw : (uint32_t)g_hw;
    AcquireSRWLockExclusive(&g_alloc);
    if ((uint32_t)g_hw < hw) memset(g_o + g_hw, 0, (size_t)(hw - g_hw) * sizeof(VObj));
    for (uint32_t i = 1; i < h->hw; ++i) {
        const VObj *s = &snap[i];
        VObj *o = &g_o[i];
        if (!obj_live(s)) { if (obj_live(o)) ++kept_now; continue; }
        if (o->type == s->type && o->gen == s->gen) {
            if (!o->dead) { if (o->type == VS_SEMA) { o->word = s->word; ++sema_counts; } continue; }   /* the same object, alive: as it is now (a semaphore gets the slot's count) */
            *o = *s; fresh_state(o); o->dead = 0; ++revived;
        } else {
            if (obj_live(o)) ++conflicts;
            *o = *s; fresh_state(o); ++revived;
        }
    }
    g_hw = (LONG)hw;
    rebuild_free();
    uint32_t kicked = sema_kick_all();
    ReleaseSRWLockExclusive(&g_alloc);
    _snprintf_s(note, nn, _TRUNCATE, "vsync (same process): %u destroyed objects brought back, %u objects made after the save kept, %u index conflicts; %u semaphore counts set, %u waiters served; now %ld mutex %ld cond %ld attr %ld rwlock %ld sema, %ld kept destroyed",
                revived, kept_now, conflicts, sema_counts, kicked, g_live[VS_MUTEX], g_live[VS_COND], g_live[VS_MATTR], g_live[VS_RWLOCK], g_live[VS_SEMA], g_quar);
}
/* Another process: the slot's table replaces this one. Objects this process's threads still name
 * from memory a load does not restore (stacks, registers: ext) move to free indices, those references
 * rewritten. Lock holders keep what their cells say now; condition waiters move to the queue of the
 * condition their cell names now (no wakeup); lock waiters wake and re-read their cell. */
static const unsigned long *g_tid_a, *g_tid_b; static int g_ntid;   /* transplanted threads: slot tid -> tid now */
static int tid_index(const unsigned long *list, unsigned long tid) { for (int i = 0; i < g_ntid; ++i) if (list[i] == tid) return i; return -1; }
static VThr *vthr_of(unsigned long tid) { LONG n = g_nvt < VTHR_MAX ? g_nvt : VTHR_MAX - 1; for (LONG i = n; i >= 1; --i) if (g_vt[i].tid == tid) return &g_vt[i]; return NULL; }
void vs_set_thread_wait(unsigned long tid, uint64_t c1, uint64_t c2) {
    VThr *t = vthr_of(tid);
    if (!t || !t->wkind) return;
    t->wc1 = (uint64_t *)c1; t->wc2 = (uint64_t *)c2;
    if (t->wkind == 2) t->wcell = (uint64_t *)c1;
}
static void restore_replace(const VsHdr *h, const VObj *snap, const VsRange *ext, int next, char *note, size_t nn) {
    static VsHold live[HOLD_CAP];
    uint32_t nl = collect_holds(live, HOLD_CAP);
    AcquireSRWLockExclusive(&g_alloc);
    uint32_t old_hw = (uint32_t)g_hw;
    if (!g_cur) g_cur = (VObj *)VirtualAlloc(NULL, (SIZE_T)VS_CAP * sizeof(VObj), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    memcpy(g_cur, g_o, (size_t)old_hw * sizeof(VObj));
    static uint64_t refs[8192]; uint32_t nref = 0, nslots = 0;
    for (int r = 0; r < next; ++r)
        for (uint64_t k = 0; k < ext[r].n; ++k) {
            uint64_t v = ext[r].p[k]; int t;
            if ((v & VS_TAGMASK) != VS_TAG || !vs_is_handle(v, &t) || !get(v, t)) continue;
            ++nslots;
            if (nref < 8192 && !find_ref(refs, nref, v)) refs[nref++] = v;
        }
    memcpy(g_o, snap, (size_t)h->hw * sizeof(VObj));
    if (old_hw > h->hw) memset(g_o + h->hw, 0, (size_t)(old_hw - h->hw) * sizeof(VObj));
    g_hw = (LONG)h->hw; g_seq = (LONG)h->seq; g_snap_seq = (LONG)h->seq;
    g_table_id = new_table_id();
    for (uint32_t i = 1; i < h->hw; ++i) fresh_state(&g_o[i]);
    static uint64_t from[8192], to[8192]; uint32_t nmove = 0, rewritten = 0;
    for (uint32_t k = 0; k < nref; ++k) {   /* every referenced object of now gets a fresh index */
        uint32_t i = (uint32_t)(refs[k] & 0xffffff), j;
        for (j = 1; j < (uint32_t)g_hw && g_o[j].type != VS_FREE; ++j) {}
        if (j == (uint32_t)g_hw) { if (j >= VS_CAP) break; memset(&g_o[j], 0, sizeof(VObj)); ++g_hw; }
        g_o[j] = g_cur[i]; fresh_state(&g_o[j]);
        g_o[j].seq = (uint32_t)InterlockedIncrement(&g_seq);
        from[nmove] = refs[k]; to[nmove] = mk(j, g_cur[i].type, g_cur[i].gen); ++nmove;
    }
    for (int r = 0; r < next && nmove; ++r)
        for (uint64_t k = 0; k < ext[r].n; ++k)
            for (uint32_t m = 0; m < nmove; ++m) if (ext[r].p[k] == from[m]) { ext[r].p[k] = to[m]; ++rewritten; break; }
    rebuild_free();
    ReleaseSRWLockExclusive(&g_alloc);
    LONG nt = g_nvt < VTHR_MAX ? g_nvt : VTHR_MAX - 1;
    for (LONG i = 1; i <= nt; ++i) g_vt[i].nrd = 0;
    /* transplanted threads keep the slot's holds; the others' holds follow their cells */
    uint32_t same = 0, moved = 0, lost = 0, from_slot = 0, slot_lost = 0;
    char miss[300]; int mn = 0; miss[0] = 0;
    const VsHold *sh = (const VsHold *)((const uint8_t *)snap + (uint64_t)h->hw * sizeof(VObj));
    for (uint32_t k = 0; k < h->nholds; ++k) {
        int ti = tid_index(g_tid_a, sh[k].tid);
        if (ti < 0) continue;
        VObj *o = get(sh[k].h, sh[k].type == HOLD_MUTEX ? VS_MUTEX : VS_RWLOCK);
        unsigned long tb = g_tid_b[ti];
        if (o && sh[k].type == HOLD_MUTEX && !o->word) { o->word = 2; o->owner = tb; o->count = (LONG)sh[k].count; o->cell = sh[k].cell; ++from_slot; }
        else if (o && sh[k].type == HOLD_WRITE && !o->word) { o->word = -1; o->owner = tb; o->cell = sh[k].cell; ++from_slot; }
        else if (o && sh[k].type == HOLD_READ && o->word >= 0) {
            VThr *t = vthr_of(tb);
            if (t && t->nrd < RD_MAX) { o->word += (LONG)sh[k].count; RHold *r = &t->rd[t->nrd++]; r->h = sh[k].h; r->cell = sh[k].cell; r->count = sh[k].count; ++from_slot; }
            else ++slot_lost;
        } else ++slot_lost;
    }
    for (uint32_t k = 0; k < nl; ++k) {
        VsHold *l = &live[k];
        if (tid_index(g_tid_b, l->tid) >= 0) continue;
        uint64_t v = 0;
        __try { v = *(volatile uint64_t *)l->cell; } __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
        VObj *o = get(v, l->type == HOLD_MUTEX ? VS_MUTEX : VS_RWLOCK);
        int ok = 0;
        if (o && l->type == HOLD_MUTEX && !o->word) { o->word = 2; o->owner = l->tid; o->count = (LONG)l->count; o->cell = l->cell; ok = 1; }
        else if (o && l->type == HOLD_WRITE && !o->word) { o->word = -1; o->owner = l->tid; o->cell = l->cell; ok = 1; }
        else if (o && l->type == HOLD_READ && o->word >= 0) {
            o->word += (LONG)l->count;
            for (LONG i = 1; i <= nt; ++i) if (g_vt[i].tid == l->tid && g_vt[i].nrd < RD_MAX) { RHold *r = &g_vt[i].rd[g_vt[i].nrd++]; r->h = v; r->cell = l->cell; r->count = l->count; break; }
            ok = 1;
        }
        if (ok) { if (v == l->h) ++same; else ++moved; }
        else { ++lost; if (mn < 250) mn += _snprintf_s(miss + mn, sizeof miss - mn, _TRUNCATE, " [type %u tid %u cell 0x%llx: 0x%llx]", l->type, l->tid, l->cell, v); }
    }
    /* condition waiters: into the queue of what their cell names now; lock waiters: re-read */
    uint32_t requeued = 0, cwoken = 0, lwoken = 0, srequeued = 0, sfailed = 0;
    for (LONG i = 1; i <= nt; ++i) {
        VThr *t = &g_vt[i];
        if (!t->wait_addr) continue;
        if (t->wait_addr == &t->signaled && t->wkind == 4) {   /* semaphore: into the queue of the id its slot thread waited on (wc1) */
            VObj *s = sema_get((uint32_t)(uint64_t)t->wc1);
            t->qnext = 0;
            if (s) {
                t->sneed = (int32_t)(uint64_t)t->wc2; t->qobj = (uint32_t)(s - g_o);
                if (s->qtail) g_vt[s->qtail].qnext = (uint32_t)i; else s->qhead = (uint32_t)i;
                s->qtail = (uint32_t)i; ++srequeued;
            } else { t->qobj = 0; t->sres = oerr(85); t->signaled = 1; ++sfailed; }   /* nothing to wait on: as if cancelled */
            continue;
        }
        if (t->wait_addr == &t->signaled && t->qobj) {
            uint64_t v = 0;
            __try { v = t->wcell ? *(volatile uint64_t *)t->wcell : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
            VObj *c = get(v, VS_COND);
            if (c) {
                t->qnext = 0; t->qobj = (uint32_t)(c - g_o);
                if (c->qtail) g_vt[c->qtail].qnext = (uint32_t)i; else c->qhead = (uint32_t)i;
                c->qtail = (uint32_t)i; ++requeued; continue;
            }
            t->qobj = 0; ++cwoken;   /* nothing to wait on: spurious wakeup */
        } else ++lwoken;
    }
    uint32_t skicked = sema_kick_all();
    InterlockedIncrement(&g_epoch);
    if (cwoken) InterlockedIncrement(&g_cepoch);
    for (LONG i = 1; i <= nt; ++i) {
        VThr *t = &g_vt[i];
        if (t->wait_addr && (t->wait_addr != &t->signaled || !t->qobj || t->signaled)) WakeByAddressAll((PVOID)t->wait_addr);
    }
    _snprintf_s(note, nn, _TRUNCATE, "vsync (other process): table %u entries (this process had %u); refs outside restored memory %u (%u objects moved, %u values rewritten); holds of transplanted threads from the slot %u (%u not applied); other live holds %u: %u same handle, %u via cell, %u lost%s; waiters: %u conditions re-queued, %u woken (spurious), %u lock waiters re-read; semaphores: %u waiters re-queued, %u cancelled (no such id), %u served",
                (uint32_t)g_hw, old_hw, nslots, nmove, rewritten, from_slot, slot_lost, nl, same, moved, lost, miss, requeued, cwoken, lwoken, srequeued, sfailed, skicked);
}
int vs_restore(const uint8_t *b, uint64_t len, const VsRange *ext, int next, int same_process, const unsigned long *tid_a, const unsigned long *tid_b, int ntid, char *note, size_t nn) {
    g_tid_a = tid_a; g_tid_b = tid_b; g_ntid = tid_a && tid_b ? ntid : 0;
    const VsHdr *h = (const VsHdr *)b;
    if (b && len >= sizeof *h && h->magic == 0x434E5953u && h->version != VS_TABLE_VERSION) { _snprintf_s(note, nn, _TRUNCATE, "vsync: slot table version %u, this DLL has %d", h->version, VS_TABLE_VERSION); return 0; }
    if (!b || len < sizeof *h || h->magic != 0x434E5953u || len < sizeof *h + (uint64_t)h->hw * sizeof(VObj) || h->hw > VS_CAP) {
        _snprintf_s(note, nn, _TRUNCATE, "vsync: slot has no sync table"); return 0;
    }
    const VObj *snap = (const VObj *)(b + sizeof *h);
    if (same_process) restore_merge(h, snap, note, nn);
    else restore_replace(h, snap, ext, next, note, nn);
    return 1;
}

/* ---- cost ---- */
extern int64_t sysv_call2(void *fn, uint64_t a, uint64_t b);
void vs_bench(uint8_t *exe, uint64_t rva_lock, uint64_t rva_unlock, uint64_t rva_init, double *ours, double *orig) {
    LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f);
    const int N = 200000;
    uint64_t cell = 0;
    mutex_init(&cell, NULL);
    QueryPerformanceCounter(&a);
    for (int i = 0; i < N; ++i) { sysv_call2((void *)vs_mutex_lock, (uint64_t)&cell, 0); sysv_call2((void *)vs_mutex_unlock, (uint64_t)&cell, 0); }
    QueryPerformanceCounter(&b);
    *ours = (double)(b.QuadPart - a.QuadPart) * 1e9 / f.QuadPart / N;
    mutex_destroy(&cell);
    uint64_t host = 0;
    sysv_call2(exe + rva_init, (uint64_t)&host, 0);
    QueryPerformanceCounter(&a);
    for (int i = 0; i < N; ++i) { sysv_call2(exe + rva_lock, (uint64_t)&host, 0); sysv_call2(exe + rva_unlock, (uint64_t)&host, 0); }
    QueryPerformanceCounter(&b);
    *orig = (double)(b.QuadPart - a.QuadPart) * 1e9 / f.QuadPart / N;
    /* the host mutex is left allocated (a few bytes) */
}
