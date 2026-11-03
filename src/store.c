/* Savestate slots on disk: shared page pool with dedup by XXH3-128, zstd -1 per page. See store.h.
 * Locking: g_lock (SRW) guards the index, the packs table and the stage list. The worker thread holds
 * it exclusively from store_save_pages to store_save_commit and for a whole load (game paused); the
 * background thread takes it only for short index updates. Nothing here touches the process heap
 * while the game is paused: page tables, staging and per-thread buffers are VirtualAlloc'd or made
 * at init. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "store.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "../third_party/zstd/lib/zstd.h"
#define XXH_INLINE_ALL
#include "../third_party/xxhash/xxhash.h"
#pragma comment(lib, "obj/libs/zstd.lib")

#define PAGE 4096ull
#define CHUNK 1024              /* pages per parallel task */
#define MAX_PACKS 4096
#define PACK_LIMIT (256ull << 20)
#define REC_MAGIC 0x47504242u   /* 'BBPG' */
#define F_PENDING 0x80000000u
#define F_RAW 0x40000000u
#define CS_MASK 0xFFFFu
#define F_CHECKED 0x20000000u   /* the pool check read this record back intact */
#define REC_HDR 24ull
#define MAX_THR 32

typedef struct { uint64_t lo, hi; } H128;
typedef struct { uint64_t lo, hi, loc; uint32_t cs, ref; } IEnt; /* loc: staging pointer if pending, else pack<<40 | offset */
typedef struct { HANDLE h; uint64_t size, live; int exists; } Pack;
typedef struct Stage {
    uint8_t *mem; H128 *hash; uint64_t n, bytes; uint64_t seq;
    struct Stage *next;
} Stage;
typedef struct { uint64_t addr, npages; } TReg; /* page table region */

static char g_dir[MAX_PATH];
static LogFn L;
static SRWLOCK g_lock = SRWLOCK_INIT;
static volatile LONG64 g_ram, g_ram_peak;

/* ---- memory ---- */
static void *big_alloc(uint64_t n) {
    if (!n) n = 1;
    void *p = VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p) {
        LONG64 now = InterlockedAdd64(&g_ram, (LONG64)((n + PAGE - 1) & ~(PAGE - 1)));
        for (LONG64 pk = g_ram_peak; now > pk; pk = g_ram_peak) InterlockedCompareExchange64(&g_ram_peak, now, pk);
    }
    return p;
}
static void big_free(void *p, uint64_t n) {
    if (!p) return;
    if (!n) n = 1;
    VirtualFree(p, 0, MEM_RELEASE);
    InterlockedAdd64(&g_ram, -(LONG64)((n + PAGE - 1) & ~(PAGE - 1)));
}
uint64_t store_ram_bytes(void) { return (uint64_t)g_ram; }
uint64_t store_ram_peak(void) { return (uint64_t)g_ram_peak; }
static double now_ms(void) { static LARGE_INTEGER f; LARGE_INTEGER t; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return t.QuadPart * 1000.0 / f.QuadPart; }

/* ---- index (open addressing, linear probing, backward-shift delete) ---- */
static IEnt *g_ix; static uint64_t g_ix_cap, g_ix_n;
static IEnt *ix_find(uint64_t lo, uint64_t hi) {
    uint64_t m = g_ix_cap - 1, i = lo & m;
    for (;; i = (i + 1) & m) {
        IEnt *e = &g_ix[i];
        if (!e->lo && !e->hi) return NULL;
        if (e->lo == lo && e->hi == hi) return e;
    }
}
static void ix_rehash(uint64_t cap);
static void ix_grow(uint64_t need) {
    if (g_ix && (need + g_ix_n) * 100 < g_ix_cap * 85) return;
    uint64_t cap = g_ix_cap ? g_ix_cap : (1ull << 16);
    while ((need + g_ix_n) * 100 >= cap * 85) cap <<= 1;
    ix_rehash(cap);
}
/* After a save the table was sized for every staged page; give back what dedup made unnecessary. */
static void ix_shrink(void) {
    uint64_t cap = g_ix_cap;
    while (cap > (1ull << 16) && g_ix_n * 100 < (cap >> 1) * 80) cap >>= 1;
    if (cap < g_ix_cap) ix_rehash(cap);
}
static void ix_rehash(uint64_t cap) {
    IEnt *old = g_ix; uint64_t ocap = g_ix_cap;
    g_ix = (IEnt *)big_alloc(cap * sizeof(IEnt)); g_ix_cap = cap;
    for (uint64_t k = 0; k < ocap; ++k) {
        IEnt *e = &old[k];
        if (!e->lo && !e->hi) continue;
        uint64_t i = e->lo & (cap - 1);
        while (g_ix[i].lo || g_ix[i].hi) i = (i + 1) & (cap - 1);
        g_ix[i] = *e;
    }
    big_free(old, ocap * sizeof(IEnt));
}
static IEnt *ix_insert(uint64_t lo, uint64_t hi, int *created) {
    uint64_t m = g_ix_cap - 1, i = lo & m;
    for (;; i = (i + 1) & m) {
        IEnt *e = &g_ix[i];
        if (!e->lo && !e->hi) { e->lo = lo; e->hi = hi; e->loc = 0; e->cs = 0; e->ref = 0; ++g_ix_n; *created = 1; return e; }
        if (e->lo == lo && e->hi == hi) { *created = 0; return e; }
    }
}
static void ix_remove(IEnt *e) {
    uint64_t m = g_ix_cap - 1, i = (uint64_t)(e - g_ix), j = i;
    --g_ix_n;
    for (;;) {
        g_ix[i].lo = g_ix[i].hi = 0;
        for (;;) {
            j = (j + 1) & m;
            if (!g_ix[j].lo && !g_ix[j].hi) return;
            uint64_t h = g_ix[j].lo & m;
            if (i <= j ? (i < h && h <= j) : (i < h || h <= j)) continue;
            break;
        }
        g_ix[i] = g_ix[j]; i = j;
    }
}

/* ---- packs ---- */
static Pack g_pk[MAX_PACKS];
static int g_cur = -1, g_next_pack = 1;
static void pack_path(int id, char *out) { _snprintf_s(out, MAX_PATH, _TRUNCATE, "%s\\pool\\pack_%06d.dat", g_dir, id); }
static int pread_(HANDLE h, uint64_t off, void *buf, DWORD n) {
    OVERLAPPED o; memset(&o, 0, sizeof o); o.Offset = (DWORD)off; o.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    return ReadFile(h, buf, n, &got, &o) && got == n;
}
static int pwrite_(HANDLE h, uint64_t off, const void *buf, DWORD n) {
    OVERLAPPED o; memset(&o, 0, sizeof o); o.Offset = (DWORD)off; o.OffsetHigh = (DWORD)(off >> 32);
    DWORD put = 0;
    return WriteFile(h, buf, n, &put, &o) && put == n;
}
static int pack_open(int id, int create) {
    char p[MAX_PATH]; pack_path(id, p);
    HANDLE h = CreateFileA(p, GENERIC_READ | (create ? GENERIC_WRITE : 0), FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, create ? CREATE_ALWAYS : OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    g_pk[id].h = h; g_pk[id].size = (uint64_t)sz.QuadPart; g_pk[id].live = 0; g_pk[id].exists = 1;
    if (id >= g_next_pack) g_next_pack = id + 1;
    return 1;
}
static void pack_delete(int id) {
    char p[MAX_PATH]; pack_path(id, p);
    if (g_pk[id].h) CloseHandle(g_pk[id].h);
    memset(&g_pk[id], 0, sizeof(Pack));
    DeleteFileA(p);
}
static int pack_current(void) {
    if (g_cur >= 0 && g_pk[g_cur].size < PACK_LIMIT) return g_cur;
    if (g_cur >= 0) FlushFileBuffers(g_pk[g_cur].h);
    int id = g_next_pack;
    if (id >= MAX_PACKS || !pack_open(id, 1)) { L("store: cannot create pack %d", id); return -1; }
    g_cur = id;
    return id;
}
static void entry_dead(IEnt *e) { /* stored entry whose last reference went away */
    if (!(e->cs & F_PENDING)) { int p = (int)(e->loc >> 40); uint64_t n = (e->cs & CS_MASK) + REC_HDR; if (g_pk[p].live >= n) g_pk[p].live -= n; }
    ix_remove(e);
}

/* ---- thread pools ---- */
typedef void (*Task)(void *ctx, uint64_t item, int t);
typedef struct {
    int n; HANDLE th[MAX_THR], go[MAX_THR], done;
    volatile LONG remaining; volatile LONG64 next; uint64_t items; Task fn; void *ctx; volatile LONG quit;
} Pool;
typedef struct { Pool *p; int t; } PoolArg;
static PoolArg g_pargs[2][MAX_THR];
static void pool_work(Pool *p, int t) {
    for (;;) {
        LONG64 i = InterlockedIncrement64(&p->next) - 1;
        if ((uint64_t)i >= p->items) break;
        p->fn(p->ctx, (uint64_t)i, t);
    }
}
static DWORD WINAPI pool_thread(LPVOID a) {
    PoolArg *pa = (PoolArg *)a;
    for (;;) {
        WaitForSingleObject(pa->p->go[pa->t], INFINITE);
        if (pa->p->quit) return 0;
        pool_work(pa->p, pa->t);
        if (InterlockedDecrement(&pa->p->remaining) == 0) SetEvent(pa->p->done);
    }
}
static void pool_start(Pool *p, int which, int n, int prio) {
    p->n = n < 1 ? 1 : n > MAX_THR ? MAX_THR : n;
    p->done = CreateEventA(NULL, FALSE, FALSE, NULL);
    for (int t = 1; t < p->n; ++t) {
        g_pargs[which][t].p = p; g_pargs[which][t].t = t;
        p->go[t] = CreateEventA(NULL, FALSE, FALSE, NULL);
        p->th[t] = CreateThread(NULL, 256 << 10, pool_thread, &g_pargs[which][t], 0, NULL);
        SetThreadPriority(p->th[t], prio);
    }
}
/* Runs fn(ctx, i, t) for i in [0, items) on all threads of the pool; the caller is thread 0. */
static void pool_run(Pool *p, Task fn, void *ctx, uint64_t items) {
    p->fn = fn; p->ctx = ctx; p->items = items; p->next = 0;
    p->remaining = p->n - 1;
    ResetEvent(p->done);
    for (int t = 1; t < p->n; ++t) SetEvent(p->go[t]);
    pool_work(p, 0);
    if (p->n > 1) WaitForSingleObject(p->done, INFINITE);
}
static void pool_stop(Pool *p) {
    p->quit = 1;
    for (int t = 1; t < p->n; ++t) SetEvent(p->go[t]);
    if (p->n > 1) WaitForMultipleObjects(p->n - 1, p->th + 1, TRUE, 5000);
    for (int t = 1; t < p->n; ++t) { CloseHandle(p->th[t]); CloseHandle(p->go[t]); }
    CloseHandle(p->done);
}
static Pool g_fast, g_bgp;
/* The fast pool for other work during the pause (threads made at start: nothing is created while the game is stopped). */
int store_parallel(void (*fn)(void *ctx, uint64_t item, int t), void *ctx, uint64_t items) { pool_run(&g_fast, fn, ctx, items); return g_fast.n; }
int store_threads(void) { return g_fast.n; }
static ZSTD_DCtx *g_dctx[MAX_THR];
static ZSTD_CCtx *g_cctx[MAX_THR];
static uint8_t *g_rdbuf[MAX_THR];     /* 8 KB per fast thread */

/* ---- page tables ---- */
typedef struct { const SRegion *r; uint64_t first_page, first_chunk; } RegMap;
static RegMap g_rm[2048]; static int g_nrm; static uint64_t g_npages, g_nchunks;
static int map_regions(const SRegion *r, int nr) {
    if (nr > 2048) return 0;
    g_nrm = nr; g_npages = 0; g_nchunks = 0;
    for (int i = 0; i < nr; ++i) {
        g_rm[i].r = &r[i]; g_rm[i].first_page = g_npages; g_rm[i].first_chunk = g_nchunks;
        uint64_t n = r[i].size / PAGE;
        g_npages += n; g_nchunks += (n + CHUNK - 1) / CHUNK;
    }
    return 1;
}
/* chunk -> region index, first page index within the region, page count */
static int chunk_at(uint64_t c, uint64_t *rp, uint64_t *cnt) {
    int lo = 0, hi = g_nrm - 1;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; if (g_rm[mid].first_chunk <= c) lo = mid; else hi = mid - 1; }
    uint64_t n = g_rm[lo].r->size / PAGE;
    *rp = (c - g_rm[lo].first_chunk) * CHUNK;
    *cnt = n - *rp < CHUNK ? n - *rp : CHUNK;
    return lo;
}
static H128 page_hash(const uint8_t *p) {
    const uint64_t *q = (const uint64_t *)p;
    uint64_t acc = 0;
    for (int i = 0; i < (int)(PAGE / 8); i += 8) { acc |= q[i] | q[i + 1] | q[i + 2] | q[i + 3] | q[i + 4] | q[i + 5] | q[i + 6] | q[i + 7]; if (acc) break; }
    H128 h = {0, 0};
    if (!acc) return h;
    XXH128_hash_t x = XXH3_128bits(p, PAGE);
    h.lo = x.low64; h.hi = x.high64;
    if (!h.lo && !h.hi) h.lo = 1; /* {0,0} means zero page */
    return h;
}

/* ---- save ---- */
static H128 *g_tab; static uint64_t g_tab_bytes;   /* page table of the save in progress */
static uint8_t *g_flag;                             /* 1 = page must be staged */
static Stage *g_stage_new;                          /* staged by store_save_pages, queued by commit */
static Stage *g_stages, **g_stages_tail = &g_stages;
static volatile LONG64 g_stage_seq, g_stage_done;
static volatile LONG64 g_stage_pos;
static volatile LONG64 g_cnt_zero, g_cnt_known, g_cnt_new;
static TReg g_treg[2048]; static int g_ntreg;

static void save_hash_task(void *ctx, uint64_t c, int t) {
    (void)ctx; (void)t;
    uint64_t rp, cnt; int ri = chunk_at(c, &rp, &cnt);
    const SRegion *r = g_rm[ri].r;
    uint64_t base = g_rm[ri].first_page + rp, z = 0, k = 0, nw = 0;
    for (uint64_t i = 0; i < cnt; ++i) {
        H128 h = page_hash(r->host + (rp + i) * PAGE);
        g_tab[base + i] = h;
        if (!h.lo && !h.hi) { g_flag[base + i] = 0; ++z; continue; }
        if (ix_find(h.lo, h.hi)) { g_flag[base + i] = 0; ++k; continue; }
        g_flag[base + i] = 1; ++nw;
    }
    InterlockedAdd64(&g_cnt_zero, z); InterlockedAdd64(&g_cnt_known, k); InterlockedAdd64(&g_cnt_new, nw);
}
static void save_copy_task(void *ctx, uint64_t c, int t) {
    (void)t;
    Stage *s = (Stage *)ctx;
    uint64_t rp, cnt; int ri = chunk_at(c, &rp, &cnt);
    const SRegion *r = g_rm[ri].r;
    uint64_t base = g_rm[ri].first_page + rp, mine = 0;
    for (uint64_t i = 0; i < cnt; ++i) mine += g_flag[base + i];
    if (!mine) return;
    uint64_t k = (uint64_t)InterlockedAdd64(&g_stage_pos, (LONG64)mine) - mine;
    for (uint64_t i = 0; i < cnt; ++i) {
        if (!g_flag[base + i]) continue;
        memcpy(s->mem + k * PAGE, r->host + (rp + i) * PAGE, PAGE);
        s->hash[k] = g_tab[base + i];
        ++k;
    }
}
int store_save_pages(const SRegion *r, int nr, SaveStats *st) {
    double t0 = now_ms();
    memset(st, 0, sizeof *st);
    AcquireSRWLockExclusive(&g_lock);
    if (!map_regions(r, nr)) { ReleaseSRWLockExclusive(&g_lock); L("store: too many regions"); return 0; }
    g_tab_bytes = g_npages * sizeof(H128);
    g_tab = (H128 *)big_alloc(g_tab_bytes);
    g_flag = (uint8_t *)big_alloc(g_npages);
    if (!g_tab || !g_flag) { big_free(g_tab, g_tab_bytes); big_free(g_flag, g_npages); g_tab = NULL; g_flag = NULL; ReleaseSRWLockExclusive(&g_lock); L("store: no memory for the page table"); return 0; }
    g_ntreg = nr;
    for (int i = 0; i < nr; ++i) { g_treg[i].addr = r[i].addr; g_treg[i].npages = r[i].size / PAGE; }
    g_cnt_zero = g_cnt_known = g_cnt_new = 0;
    pool_run(&g_fast, save_hash_task, NULL, g_nchunks);
    double t1 = now_ms();
    Stage *s = NULL;
    if (g_cnt_new) {
        s = (Stage *)big_alloc(sizeof(Stage));
        s->n = (uint64_t)g_cnt_new; s->bytes = s->n * PAGE;
        s->mem = (uint8_t *)big_alloc(s->bytes);
        s->hash = (H128 *)big_alloc(s->n * sizeof(H128));
        if (!s->mem || !s->hash) {
            big_free(s->mem, s->bytes); big_free(s->hash, s->n * sizeof(H128)); big_free(s, sizeof(Stage));
            big_free(g_tab, g_tab_bytes); big_free(g_flag, g_npages); g_tab = NULL; g_flag = NULL;
            ReleaseSRWLockExclusive(&g_lock);
            L("store: no memory to stage %llu MB", (uint64_t)g_cnt_new * PAGE >> 20);
            return 0;
        }
        g_stage_pos = 0;
        pool_run(&g_fast, save_copy_task, s, g_nchunks);
    }
    g_stage_new = s;
    st->pages = g_npages; st->zero = (uint64_t)g_cnt_zero; st->known = (uint64_t)g_cnt_known; st->staged = (uint64_t)g_cnt_new;
    st->staged_bytes = st->staged * PAGE; st->hash_ms = t1 - t0; st->copy_ms = now_ms() - t1;
    return 1;
}

/* ---- slot files ---- */
typedef struct { int state; uint64_t need_seq; char *meta; } SlotMem; /* state: 0 none, 1 complete, 2 pending */
static SlotMem g_slot[STORE_MAX_SLOTS + 1];
static volatile LONG g_check_bad[STORE_MAX_SLOTS + 1];   /* damaged pages found by the pool check */
static volatile LONG g_slot_gen[STORE_MAX_SLOTS + 1];   /* +1 at every save of the slot: a check of its old table is void */
static void slot_dir(int slot, const char *suffix, char *out) { _snprintf_s(out, MAX_PATH, _TRUNCATE, "%s\\slot%02d%s", g_dir, slot, suffix); }
static int write_file(const char *dir, const char *name, const void *data, uint64_t len) {
    char p[MAX_PATH]; _snprintf_s(p, MAX_PATH, _TRUNCATE, "%s\\%s", dir, name);
    HANDLE f = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    int ok = 1;
    for (uint64_t off = 0; off < len && ok; ) { DWORD n = (DWORD)(len - off > (1u << 30) ? (1u << 30) : len - off), w; ok = WriteFile(f, (const uint8_t *)data + off, n, &w, NULL) && w == n; off += n; }
    FlushFileBuffers(f); CloseHandle(f);
    return ok;
}
static uint8_t *read_file(const char *path, uint64_t *len) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
    uint8_t *b = (uint8_t *)malloc((size_t)sz.QuadPart + 1);
    DWORD got = 0;
    if (b && !ReadFile(f, b, (DWORD)sz.QuadPart, &got, NULL)) { free(b); b = NULL; }
    CloseHandle(f);
    if (b) { b[got] = 0; *len = got; }
    return b;
}
static void rm_tree(const char *dir) {
    char pat[MAX_PATH], p[MAX_PATH]; WIN32_FIND_DATAA fd;
    _snprintf_s(pat, MAX_PATH, _TRUNCATE, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            _snprintf_s(p, MAX_PATH, _TRUNCATE, "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm_tree(p); else DeleteFileA(p);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}
/* pages.bin: zstd frame of {u32 'BBPT', u32 version 1, u32 nregions, u32 pad, u64 npages, TReg[n], H128[npages]} */
static int write_table(const char *dir, const TReg *reg, int nreg, const H128 *tab, uint64_t npages) {
    uint64_t raw = 24 + nreg * sizeof(TReg) + npages * sizeof(H128);
    uint8_t *buf = (uint8_t *)malloc(raw);
    if (!buf) return 0;
    uint32_t h[4] = {0x54504242u, 1, (uint32_t)nreg, 0};
    memcpy(buf, h, 16); memcpy(buf + 16, &npages, 8);
    memcpy(buf + 24, reg, nreg * sizeof(TReg)); memcpy(buf + 24 + nreg * sizeof(TReg), tab, npages * sizeof(H128));
    size_t cap = ZSTD_compressBound(raw);
    uint8_t *out = (uint8_t *)malloc(cap);
    size_t c = out ? ZSTD_compress(out, cap, buf, raw, 1) : 0;
    int ok = out && !ZSTD_isError(c) && write_file(dir, "pages.bin", out, c);
    free(buf); free(out);
    return ok;
}
/* Reads a slot's page table: malloc'd buffer, regions and hashes point into it. */
static uint8_t *read_table(int slot, const char *suffix, TReg **reg, int *nreg, H128 **tab, uint64_t *npages) {
    char d[MAX_PATH], p[MAX_PATH]; uint64_t len;
    slot_dir(slot, suffix, d); _snprintf_s(p, MAX_PATH, _TRUNCATE, "%s\\pages.bin", d);
    uint8_t *c = read_file(p, &len);
    if (!c) return NULL;
    unsigned long long raw = ZSTD_getFrameContentSize(c, len);
    if (raw == ZSTD_CONTENTSIZE_ERROR || raw == ZSTD_CONTENTSIZE_UNKNOWN || raw < 24) { free(c); return NULL; }
    uint8_t *b = (uint8_t *)malloc((size_t)raw);
    size_t got = b ? ZSTD_decompress(b, (size_t)raw, c, len) : 0;
    free(c);
    if (!b || ZSTD_isError(got) || got != raw || ((uint32_t *)b)[0] != 0x54504242u) { free(b); return NULL; }
    *nreg = (int)((uint32_t *)b)[2]; memcpy(npages, b + 16, 8);
    *reg = (TReg *)(b + 24); *tab = (H128 *)(b + 24 + *nreg * sizeof(TReg));
    if (24 + *nreg * sizeof(TReg) + *npages * sizeof(H128) != raw) { free(b); return NULL; }
    return b;
}
static void table_ref(const H128 *tab, uint64_t n, int delta, uint64_t *missing) {
    for (uint64_t i = 0; i < n; ++i) {
        if (!tab[i].lo && !tab[i].hi) continue;
        IEnt *e = ix_find(tab[i].lo, tab[i].hi);
        if (!e) { if (missing) ++*missing; continue; }
        if (delta > 0) ++e->ref;
        else if (e->ref && --e->ref == 0 && !(e->cs & F_PENDING)) entry_dead(e);
    }
}

static HANDLE g_bg_th, g_bg_ev; static volatile LONG g_bg_quit, g_bg_busy;
static volatile LONG g_dropped_slot;   /* last slot dropped by a failed background write (store_take_dropped) */
static void write_index(void);
void store_save_abort(void) {
    Stage *s = g_stage_new; g_stage_new = NULL;
    if (s) { big_free(s->mem, s->bytes); big_free(s->hash, s->n * sizeof(H128)); big_free(s, sizeof(Stage)); }
    big_free(g_tab, g_tab_bytes); big_free(g_flag, g_npages); g_tab = NULL; g_flag = NULL;
    ReleaseSRWLockExclusive(&g_lock);
}

int store_save_commit(int slot, const char *meta_json, const void *layout, uint32_t layout_len,
                      const void *threads, uint32_t threads_len, const void *objs, uint32_t objs_len,
                      const char *fallback_dir) {
    double t0 = now_ms();
    Stage *s = g_stage_new; g_stage_new = NULL;
    /* index the staged pages (duplicates inside one save are staged twice, indexed once) */
    if (s) {
        ix_grow(s->n);
        for (uint64_t k = 0; k < s->n; ++k) {
            int created;
            IEnt *e = ix_insert(s->hash[k].lo, s->hash[k].hi, &created);
            if (created) { e->loc = (uint64_t)(s->mem + k * PAGE); e->cs = F_PENDING; }
        }
        s->seq = (uint64_t)InterlockedIncrement64(&g_stage_seq);
        *g_stages_tail = s; g_stages_tail = &s->next;
    }
    uint64_t missing = 0;
    table_ref(g_tab, g_npages, +1, &missing);
    /* previous contents of the slot */
    TReg *oreg; int onreg; H128 *otab; uint64_t on;
    uint8_t *old = read_table(slot, "", &oreg, &onreg, &otab, &on);
    if (old) { table_ref(otab, on, -1, NULL); free(old); }
    char dn[MAX_PATH], dd[MAX_PATH], fb[MAX_PATH];
    slot_dir(slot, ".new", dn); slot_dir(slot, "", dd);
    rm_tree(dn);
    CreateDirectoryA(dn, NULL);
    int ok = write_table(dn, g_treg, g_ntreg, g_tab, g_npages);
    if (layout) ok &= write_file(dn, "layout.bin", layout, layout_len);
    if (threads) {
        size_t cap = ZSTD_compressBound(threads_len); void *c = malloc(cap);
        size_t n = c ? ZSTD_compress(c, cap, threads, threads_len, 3) : 0;
        if (c && !ZSTD_isError(n)) ok &= write_file(dn, "threads.bin", c, n);
        free(c);
    }
    if (objs) ok &= write_file(dn, "hostobjs.bin", objs, objs_len);
    if (fallback_dir && *fallback_dir) {
        static const char *const files[] = {"userdata0001", "backup0001", "userdata0010", "backup0010"};
        _snprintf_s(fb, MAX_PATH, _TRUNCATE, "%s\\fallback", dn); CreateDirectoryA(fb, NULL);
        for (int i = 0; i < 4; ++i) {
            char a[MAX_PATH], b[MAX_PATH];
            _snprintf_s(a, MAX_PATH, _TRUNCATE, "%s\\%s", fallback_dir, files[i]); _snprintf_s(b, MAX_PATH, _TRUNCATE, "%s\\%s", fb, files[i]);
            CopyFileA(a, b, FALSE);
        }
    }
    int pending = s || 0;
    /* a slot is complete once every stage up to now is written (its pending pages are in them) */
    uint64_t need = (uint64_t)g_stage_seq;
    if (need > (uint64_t)g_stage_done) pending = 1;
    free(g_slot[slot].meta); g_slot[slot].meta = _strdup(meta_json ? meta_json : "{}");
    char *m = g_slot[slot].meta;
    if (!pending) { char *q = strstr(m, "\"complete\": false"); if (q) memcpy(q, "\"complete\": true ", 17); }
    ok &= write_file(dn, "metadata.json", m, strlen(m));
    rm_tree(dd);
    ok &= MoveFileExA(dn, dd, 0);
    g_slot[slot].state = ok ? (pending ? 2 : 1) : 0;
    g_slot[slot].need_seq = need;
    InterlockedIncrement(&g_slot_gen[slot]); g_check_bad[slot] = 0;
    big_free(g_tab, g_tab_bytes); big_free(g_flag, g_npages); g_tab = NULL; g_flag = NULL;
    ReleaseSRWLockExclusive(&g_lock);
    if (missing) L("store: BUG: %llu pages of the new table are not in the index", missing);
    L("store: slot %d written (%s) in %.0f ms; %s", slot, ok ? "ok" : "ERRORS", now_ms() - t0, pending ? "pages are being compressed in the background" : "complete");
    SetEvent(g_bg_ev);
    if (!pending) write_index();
    return ok;
}

/* ---- load ---- */
static uint8_t *g_ld_buf; static TReg *g_ld_reg; static int g_ld_nreg; static H128 *g_ld_tab; static uint64_t g_ld_n;
static uint64_t g_ld_first[2048];         /* first table index of each table region */
static int g_ld_map[2048];                /* table region -> current region (or -1) */
static uint8_t *g_chg;                    /* per current page: 1 changed */
static volatile LONG64 g_l_changed, g_l_zero, g_l_stage, g_l_disk, g_l_bytes, g_l_err;

/* Packs a load reads are mapped (read-only views) before the pause and the slot's packs are prefetched
 * into the file cache while the game still runs: in the pause a page is a decompression from memory, not
 * a ReadFile per page. A pack that grew or appeared after the mapping is read with ReadFile. */
static uint8_t *g_view[MAX_PACKS]; static uint64_t g_view_len[MAX_PACKS];
static double g_prefetch_ms; static uint64_t g_prefetch_bytes;
static void views_open(void) {
    double t0 = now_ms();
    g_prefetch_bytes = 0; g_prefetch_ms = 0;
    AcquireSRWLockShared(&g_lock);
    for (int p = 0; p < MAX_PACKS; ++p) {
        if (!g_pk[p].exists || !g_pk[p].h || !g_pk[p].size || g_view[p]) continue;
        HANDLE m = CreateFileMappingA(g_pk[p].h, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!m) continue;
        g_view[p] = (uint8_t *)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
        CloseHandle(m);
        if (!g_view[p]) continue;
        g_view_len[p] = g_pk[p].size;
    }
    ReleaseSRWLockShared(&g_lock);
    g_prefetch_ms = now_ms() - t0;
}
/* A load that will write most of memory (another process or location): the slot's packs into the file
 * cache while the game still runs. */
void store_load_prefetch(void) {
    static uint8_t used[MAX_PACKS];
    double t0 = now_ms();
    memset(used, 0, sizeof used);
    AcquireSRWLockShared(&g_lock);
    for (uint64_t i = 0; i < g_ld_n; ++i) {
        if (!g_ld_tab[i].lo && !g_ld_tab[i].hi) continue;
        IEnt *e = ix_find(g_ld_tab[i].lo, g_ld_tab[i].hi);
        if (e && !(e->cs & F_PENDING)) used[e->loc >> 40] = 1;
    }
    ReleaseSRWLockShared(&g_lock);
    static WIN32_MEMORY_RANGE_ENTRY pre[MAX_PACKS]; int npre = 0;
    for (int p = 0; p < MAX_PACKS; ++p)
        if (used[p] && g_view[p]) { pre[npre].VirtualAddress = g_view[p]; pre[npre].NumberOfBytes = g_view_len[p]; g_prefetch_bytes += g_view_len[p]; ++npre; }
    if (npre) PrefetchVirtualMemory(GetCurrentProcess(), npre, pre, 0);
    g_prefetch_ms += now_ms() - t0;
}
static void views_close(void) {
    for (int p = 0; p < MAX_PACKS; ++p) if (g_view[p]) { UnmapViewOfFile(g_view[p]); g_view[p] = NULL; g_view_len[p] = 0; }
}
int store_load_prepare(int slot, void **layout, uint32_t *layout_len) {
    *layout = NULL; *layout_len = 0;
    if (slot < 1 || slot > STORE_MAX_SLOTS || !g_slot[slot].state) return 0;
    g_ld_buf = read_table(slot, "", &g_ld_reg, &g_ld_nreg, &g_ld_tab, &g_ld_n);
    if (!g_ld_buf) { L("store: slot %d: cannot read pages.bin", slot); return 0; }
    views_open();
    uint64_t f = 0;
    for (int i = 0; i < g_ld_nreg && i < 2048; ++i) { g_ld_first[i] = f; f += g_ld_reg[i].npages; }
    char d[MAX_PATH], p[MAX_PATH]; uint64_t len = 0;
    slot_dir(slot, "", d); _snprintf_s(p, MAX_PATH, _TRUNCATE, "%s\\layout.bin", d);
    uint8_t *lb = read_file(p, &len);
    if (lb) { *layout = lb; *layout_len = (uint32_t)len; }
    return 1;
}
void store_load_done(void) { free(g_ld_buf); g_ld_buf = NULL; views_close(); }
void store_load_prepare_stats(double *ms, uint64_t *bytes) { *ms = g_prefetch_ms; *bytes = g_prefetch_bytes; }
/* threads.bin of a slot (zstd), decompressed; caller frees. */
void *store_read_threads(int slot, uint64_t *len) {
    char d[MAX_PATH], p[MAX_PATH]; uint64_t n = 0;
    *len = 0;
    if (slot < 1 || slot > STORE_MAX_SLOTS) return NULL;
    slot_dir(slot, "", d); _snprintf_s(p, MAX_PATH, _TRUNCATE, "%s\\threads.bin", d);
    uint8_t *c = read_file(p, &n);
    if (!c) return NULL;
    unsigned long long sz = ZSTD_getFrameContentSize(c, n);
    void *out = (sz == ZSTD_CONTENTSIZE_ERROR || sz == ZSTD_CONTENTSIZE_UNKNOWN) ? NULL : malloc(sz ? sz : 1);
    if (out && ZSTD_isError(ZSTD_decompress(out, sz, c, n))) { free(out); out = NULL; }
    free(c);
    if (out) *len = sz;
    return out;
}

static int fetch_page(const H128 *h, uint8_t *dst, int t) {
    IEnt *e = ix_find(h->lo, h->hi);
    if (!e) return 0;
    if (e->cs & F_PENDING) { memcpy(dst, (const void *)e->loc, PAGE); InterlockedIncrement64(&g_l_stage); return 1; }
    int p = (int)(e->loc >> 40); uint64_t off = e->loc & ((1ull << 40) - 1); uint32_t cs = e->cs & CS_MASK;
    if (!g_pk[p].h) return 0;
    InterlockedIncrement64(&g_l_disk); InterlockedAdd64(&g_l_bytes, cs);
    if (g_view[p] && off + (e->cs & F_RAW ? PAGE : cs) <= g_view_len[p]) {
        const uint8_t *src = g_view[p] + off;
        __try {
            if (e->cs & F_RAW) { memcpy(dst, src, PAGE); return 1; }
            size_t n = ZSTD_decompressDCtx(g_dctx[t], dst, PAGE, src, cs);
            return !ZSTD_isError(n) && n == PAGE;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}   /* in-page error: read it the usual way */
    }
    if (e->cs & F_RAW) return pread_(g_pk[p].h, off, dst, (DWORD)PAGE);
    if (!pread_(g_pk[p].h, off, g_rdbuf[t], cs)) return 0;
    size_t n = ZSTD_decompressDCtx(g_dctx[t], dst, PAGE, g_rdbuf[t], cs);
    return !ZSTD_isError(n) && n == PAGE;
}
/* Pass 1 marks the pages that differ from the slot (current hash kept); the caller then
 * invalidates those ranges in the GPU caches, which may first download GPU-written data into them
 * (port readbacks, Relaxed mode); pass 2 writes the slot's pages over whatever is there now. */
static H128 *g_curh;
static volatile LONG64 g_l_gpu;
static int table_region(int ri) { for (int i = 0; i < g_ld_nreg; ++i) if (g_ld_map[i] == ri) return i; return -1; }
static void compare_task(void *ctx, uint64_t c, int t) {
    (void)ctx; (void)t;
    uint64_t rp, cnt; int ri = chunk_at(c, &rp, &cnt), tr = table_region(ri);
    if (tr < 0) return;
    const SRegion *r = g_rm[ri].r;
    uint64_t base = g_rm[ri].first_page + rp, tb = g_ld_first[tr] + rp, ch = 0;
    for (uint64_t i = 0; i < cnt; ++i) {
        H128 cur = page_hash(r->host + (rp + i) * PAGE);
        const H128 *want = &g_ld_tab[tb + i];
        if (cur.lo == want->lo && cur.hi == want->hi) continue;
        g_chg[base + i] = 1; g_curh[base + i] = cur; ++ch;
    }
    InterlockedAdd64(&g_l_changed, ch);
}
static void write_task(void *ctx, uint64_t c, int t) {
    (void)ctx;
    uint64_t rp, cnt; int ri = chunk_at(c, &rp, &cnt), tr = table_region(ri);
    if (tr < 0) return;
    const SRegion *r = g_rm[ri].r;
    uint64_t base = g_rm[ri].first_page + rp, tb = g_ld_first[tr] + rp, z = 0, gpu = 0;
    for (uint64_t i = 0; i < cnt; ++i) {
        if (!g_chg[base + i]) continue;
        uint8_t *pg = r->host + (rp + i) * PAGE;
        H128 now = page_hash(pg);
        if (now.lo != g_curh[base + i].lo || now.hi != g_curh[base + i].hi) ++gpu;
        const H128 *want = &g_ld_tab[tb + i];
        if (!want->lo && !want->hi) { memset(pg, 0, PAGE); ++z; continue; }
        if (!fetch_page(want, pg, t)) InterlockedIncrement64(&g_l_err);
    }
    InterlockedAdd64(&g_l_zero, z); InterlockedAdd64(&g_l_gpu, gpu);
}
static void for_ranges(const SRegion *r, int nr, DirtyFn fn) {
    for (int ri = 0; ri < nr; ++ri) {
        uint64_t n = r[ri].size / PAGE, b = g_rm[ri].first_page;
        for (uint64_t i = 0; i < n; ) {
            if (!g_chg[b + i]) { ++i; continue; }
            uint64_t j = i; while (j < n && g_chg[b + j]) ++j;
            fn(r[ri].addr + i * PAGE, (j - i) * PAGE);
            i = j;
        }
    }
}
int store_load_pages(const SRegion *r, int nr, DirtyFn before_write, DirtyFn dirty, LoadStats *st) {
    double t0 = now_ms();
    memset(st, 0, sizeof *st);
    AcquireSRWLockExclusive(&g_lock);
    if (!g_ld_buf || !map_regions(r, nr)) { ReleaseSRWLockExclusive(&g_lock); return 0; }
    for (int i = 0; i < g_ld_nreg; ++i) {
        g_ld_map[i] = -1;
        for (int j = 0; j < nr; ++j) if (r[j].addr == g_ld_reg[i].addr && r[j].size == g_ld_reg[i].npages * PAGE) { g_ld_map[i] = j; break; }
        if (g_ld_map[i] < 0) L("store: region 0x%llx (%llu pages) of the slot is not present now, skipped", g_ld_reg[i].addr, g_ld_reg[i].npages);
    }
    g_chg = (uint8_t *)big_alloc(g_npages);
    g_curh = (H128 *)big_alloc(g_npages * sizeof(H128));
    if (!g_chg || !g_curh) { big_free(g_chg, g_npages); big_free(g_curh, g_npages * sizeof(H128)); g_chg = NULL; g_curh = NULL; ReleaseSRWLockExclusive(&g_lock); return 0; }
    g_l_changed = g_l_zero = g_l_stage = g_l_disk = g_l_bytes = g_l_err = g_l_gpu = 0;
    pool_run(&g_fast, compare_task, NULL, g_nchunks);
    double t1 = now_ms();
    if (before_write) for_ranges(r, nr, before_write);
    double t2 = now_ms();
    pool_run(&g_fast, write_task, NULL, g_nchunks);
    for_ranges(r, nr, dirty);
    big_free(g_chg, g_npages); g_chg = NULL;
    big_free(g_curh, g_npages * sizeof(H128)); g_curh = NULL;
    ReleaseSRWLockExclusive(&g_lock);
    st->pages = g_npages; st->changed = (uint64_t)g_l_changed; st->zeroed = (uint64_t)g_l_zero; st->gpu_overwritten = (uint64_t)g_l_gpu;
    st->from_staging = (uint64_t)g_l_stage; st->from_disk = (uint64_t)g_l_disk; st->disk_bytes = (uint64_t)g_l_bytes; st->ms = now_ms() - t0;
    st->compare_ms = t1 - t0; st->before_ms = t2 - t1;
    if (g_l_err) L("store: LOAD: %lld pages could not be read from the pool", g_l_err);
    return g_l_err == 0;
}

/* Debug (command "verifyload 1"): after a load, which pages differ from the slot (the table of the load in
 * progress; call before store_load_done). Counts them and lists the first ranges. */
static volatile LONG64 g_v_diff; static uint64_t g_v_first[16]; static volatile LONG g_v_n;
static void verify_task(void *ctx, uint64_t c, int t) {
    (void)ctx; (void)t;
    uint64_t rp, cnt; int ri = chunk_at(c, &rp, &cnt), tr = table_region(ri);
    if (tr < 0) return;
    const SRegion *r = g_rm[ri].r;
    uint64_t tb = g_ld_first[tr] + rp, d = 0;
    for (uint64_t i = 0; i < cnt; ++i) {
        H128 cur = page_hash(r->host + (rp + i) * PAGE);
        const H128 *want = &g_ld_tab[tb + i];
        if (cur.lo == want->lo && cur.hi == want->hi) continue;
        ++d;
        LONG k = InterlockedIncrement(&g_v_n) - 1;
        if (k < 16) g_v_first[k] = r->addr + (rp + i) * PAGE;
    }
    InterlockedAdd64(&g_v_diff, d);
}
uint64_t store_load_verify(const SRegion *r, int nr, uint64_t *first, int nfirst) {
    AcquireSRWLockExclusive(&g_lock);
    if (!g_ld_buf || !map_regions(r, nr)) { ReleaseSRWLockExclusive(&g_lock); return ~0ull; }
    for (int i = 0; i < g_ld_nreg; ++i) {
        g_ld_map[i] = -1;
        for (int j = 0; j < nr; ++j) if (r[j].addr == g_ld_reg[i].addr && r[j].size == g_ld_reg[i].npages * PAGE) { g_ld_map[i] = j; break; }
    }
    g_v_diff = 0; g_v_n = 0;
    pool_run(&g_fast, verify_task, NULL, g_nchunks);
    ReleaseSRWLockExclusive(&g_lock);
    for (int i = 0; i < nfirst && i < 16 && i < g_v_n; ++i) first[i] = g_v_first[i];
    return (uint64_t)g_v_diff;
}

/* ---- delete / status ---- */
int store_delete(int slot) {
    if (slot < 1 || slot > STORE_MAX_SLOTS) return 0;
    AcquireSRWLockExclusive(&g_lock);
    TReg *reg; int nreg; H128 *tab; uint64_t n;
    uint8_t *b = read_table(slot, "", &reg, &nreg, &tab, &n);
    if (b) { table_ref(tab, n, -1, NULL); free(b); }
    char d[MAX_PATH]; slot_dir(slot, "", d); rm_tree(d);
    g_slot[slot].state = 0; free(g_slot[slot].meta); g_slot[slot].meta = NULL;
    InterlockedIncrement(&g_slot_gen[slot]); g_check_bad[slot] = 0;
    ReleaseSRWLockExclusive(&g_lock);
    SetEvent(g_bg_ev);
    write_index();
    return b != NULL;
}
int store_slot_exists(int slot) { return slot >= 1 && slot <= STORE_MAX_SLOTS ? g_slot[slot].state : 0; }
static uint64_t dir_bytes(const char *dir) {
    char pat[MAX_PATH], p[MAX_PATH]; WIN32_FIND_DATAA fd; uint64_t s = 0;
    _snprintf_s(pat, MAX_PATH, _TRUNCATE, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        _snprintf_s(p, MAX_PATH, _TRUNCATE, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) s += dir_bytes(p);
        else s += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return s;
}
void store_status(void) {
    AcquireSRWLockShared(&g_lock);
    uint64_t sz = 0, live = 0, pending = 0; int np = 0;
    for (int i = 0; i < MAX_PACKS; ++i) if (g_pk[i].exists) { ++np; sz += g_pk[i].size; live += g_pk[i].live; }
    for (Stage *s = g_stages; s; s = s->next) pending += s->n;
    char slots[128]; int n = 0;
    for (int i = 1; i <= STORE_MAX_SLOTS; ++i) n += _snprintf_s(slots + n, sizeof slots - n, _TRUNCATE, "%c", ".CP"[g_slot[i].state]);
    uint64_t pool_ix = g_ix_n, cap = g_ix_cap;
    ReleaseSRWLockShared(&g_lock);
    char sd[MAX_PATH]; uint64_t slot_bytes = 0;
    for (int i = 1; i <= STORE_MAX_SLOTS; ++i) { slot_dir(i, "", sd); slot_bytes += dir_bytes(sd); }
    L("store: slots [%s] (C complete, P compressing); pool %llu pages in %d packs, %llu MB on disk (%llu MB live); slot files %llu MB; staged %llu pages; index %llu/%llu; RAM %llu MB (peak %llu MB)",
      slots, pool_ix, np, sz >> 20, live >> 20, slot_bytes >> 20, pending, pool_ix, cap, store_ram_bytes() >> 20, store_ram_peak() >> 20);
}

/* ---- index file ---- */
typedef struct { uint64_t lo, hi; uint32_t pack, cs; uint64_t off; } XEnt;
static SRWLOCK g_idx_file = SRWLOCK_INIT;
static void write_index(void) {
    AcquireSRWLockExclusive(&g_idx_file);
    char p[MAX_PATH], q[MAX_PATH];
    _snprintf_s(p, MAX_PATH, _TRUNCATE, "%s\\pool\\index.bin", g_dir); _snprintf_s(q, MAX_PATH, _TRUNCATE, "%s\\pool\\index.tmp", g_dir);
    AcquireSRWLockShared(&g_lock);
    uint64_t n = 0;
    XEnt *x = (XEnt *)malloc((g_ix_n + 1) * sizeof(XEnt));
    for (uint64_t i = 0; x && i < g_ix_cap; ++i) {
        IEnt *e = &g_ix[i];
        if ((!e->lo && !e->hi) || (e->cs & F_PENDING)) continue;
        x[n].lo = e->lo; x[n].hi = e->hi; x[n].pack = (uint32_t)(e->loc >> 40); x[n].cs = e->cs; x[n].off = e->loc & ((1ull << 40) - 1); ++n;
    }
    for (int i = 0; i < MAX_PACKS; ++i) if (g_pk[i].exists && g_pk[i].h) FlushFileBuffers(g_pk[i].h);
    ReleaseSRWLockShared(&g_lock);
    if (!x) { ReleaseSRWLockExclusive(&g_idx_file); return; }
    HANDLE f = CreateFileA(q, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        uint32_t h[4] = {0x58494242u, 1, 0, 0}; DWORD w;
        WriteFile(f, h, 16, &w, NULL); WriteFile(f, &n, 8, &w, NULL);
        WriteFile(f, x, (DWORD)(n * sizeof(XEnt)), &w, NULL);
        FlushFileBuffers(f); CloseHandle(f);
        MoveFileExA(q, p, MOVEFILE_REPLACE_EXISTING);
    }
    free(x);
    ReleaseSRWLockExclusive(&g_idx_file);
}
static int read_index(void) {
    char p[MAX_PATH]; uint64_t len;
    _snprintf_s(p, MAX_PATH, _TRUNCATE, "%s\\pool\\index.bin", g_dir);
    uint8_t *b = read_file(p, &len);
    if (!b) return 0;
    uint64_t n; memcpy(&n, b + 16, 8);
    if (len < 24 || ((uint32_t *)b)[0] != 0x58494242u || 24 + n * sizeof(XEnt) != len) { free(b); return 0; }
    XEnt *x = (XEnt *)(b + 24);
    ix_grow(n);
    for (uint64_t i = 0; i < n; ++i) {
        if (x[i].pack >= MAX_PACKS || !g_pk[x[i].pack].exists || x[i].off + (x[i].cs & CS_MASK) > g_pk[x[i].pack].size) continue;
        int c; IEnt *e = ix_insert(x[i].lo, x[i].hi, &c);
        e->loc = ((uint64_t)x[i].pack << 40) | x[i].off; e->cs = x[i].cs & ~F_CHECKED;   /* checked again in every run */
    }
    free(b);
    return 1;
}
/* No index.bin: every record header of every pack. */
static void scan_packs(void) {
    uint8_t hdr[REC_HDR]; uint64_t n = 0;
    for (int id = 0; id < MAX_PACKS; ++id) {
        if (!g_pk[id].exists) continue;
        for (uint64_t off = 0; off + REC_HDR <= g_pk[id].size; ) {
            if (!pread_(g_pk[id].h, off, hdr, (DWORD)REC_HDR) || ((uint32_t *)hdr)[0] != REC_MAGIC) break;
            uint32_t cs = ((uint32_t *)hdr)[1];
            if (off + REC_HDR + (cs & CS_MASK) > g_pk[id].size) break;
            ix_grow(1);
            int c; IEnt *e = ix_insert(((uint64_t *)hdr)[1], ((uint64_t *)hdr)[2], &c);
            if (c) { e->loc = ((uint64_t)id << 40) | (off + REC_HDR); e->cs = cs; ++n; }
            off += REC_HDR + (cs & CS_MASK);
        }
    }
    L("store: index rebuilt from the packs: %llu pages", n);
}

/* ---- background: compress staged pages, compact packs ---- */
typedef struct { Stage *s; uint64_t k0, n; uint8_t *out; uint32_t *cs; uint8_t *need; } BgJob;
#define BG_CHUNK 4096
static void bg_task(void *ctx, uint64_t i, int t) {
    BgJob *j = (BgJob *)ctx;
    if (!j->need[i]) return;
    uint8_t *src = j->s->mem + (j->k0 + i) * PAGE, *dst = j->out + i * (PAGE + REC_HDR);
    size_t c = ZSTD_compressCCtx(g_cctx[t], dst + REC_HDR, PAGE, src, PAGE, 1);
    uint32_t cs;
    if (ZSTD_isError(c) || c >= PAGE - 64) { memcpy(dst + REC_HDR, src, PAGE); cs = (uint32_t)PAGE | F_RAW; }
    else cs = (uint32_t)c;
    ((uint32_t *)dst)[0] = REC_MAGIC; ((uint32_t *)dst)[1] = cs;
    ((uint64_t *)dst)[1] = j->s->hash[j->k0 + i].lo; ((uint64_t *)dst)[2] = j->s->hash[j->k0 + i].hi;
    j->cs[i] = cs;
}
static volatile LONG g_fail_writes;   /* test: the next pack writes fail (command "failwrites 1") */
static volatile LONG g_write_failed;  /* a stage could not be written: its slots are dropped in finish_stage */
static int pack_write(HANDLE h, uint64_t off, const void *buf, DWORD n) {
    if (g_fail_writes) { SetLastError(ERROR_DISK_FULL); return 0; }
    return pwrite_(h, off, buf, n);
}
static void pack_truncate(int p, uint64_t size) {
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)size;
    if (SetFilePointerEx(g_pk[p].h, li, NULL, FILE_BEGIN)) SetEndOfFile(g_pk[p].h);
}
static void bg_stage(Stage *s) {
    static uint8_t *out; static uint32_t cs[BG_CHUNK]; static uint8_t need[BG_CHUNK];
    if (!out) out = (uint8_t *)big_alloc(BG_CHUNK * (PAGE + REC_HDR));
    double t0 = now_ms(); uint64_t written = 0, wbytes = 0;
    for (uint64_t k0 = 0; k0 < s->n && !g_bg_quit; k0 += BG_CHUNK) {
        uint64_t n = s->n - k0 < BG_CHUNK ? s->n - k0 : BG_CHUNK, cnt = 0;
        AcquireSRWLockExclusive(&g_lock);
        for (uint64_t i = 0; i < n; ++i) {
            IEnt *e = ix_find(s->hash[k0 + i].lo, s->hash[k0 + i].hi);
            need[i] = e && (e->cs & F_PENDING) && e->loc == (uint64_t)(s->mem + (k0 + i) * PAGE);
            if (need[i] && !e->ref) { ix_remove(e); need[i] = 0; } /* its slot is already gone */
            cnt += need[i];
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (!cnt) continue;
        BgJob j = {s, k0, n, out, cs, need};
        pool_run(&g_bgp, bg_task, &j, n);
        /* append to the current pack (only this thread writes packs) */
        int p = pack_current();
        if (p < 0) { g_write_failed = 1; break; }
        uint64_t base = g_pk[p].size, off = base;
        static uint64_t offs[BG_CHUNK];
        static uint8_t wb[1 << 20]; uint64_t wl = 0, wstart = base;
        int wok = 1;
        for (uint64_t i = 0; i < n && wok; ++i) {
            if (!need[i]) continue;
            uint64_t len = REC_HDR + (cs[i] & CS_MASK);
            if (wl + len > sizeof wb) { wok = pack_write(g_pk[p].h, wstart, wb, (DWORD)wl); wstart += wl; wl = 0; }
            memcpy(wb + wl, out + i * (PAGE + REC_HDR), len); wl += len;
            offs[i] = off + REC_HDR; off += len;
        }
        if (wok && wl) wok = pack_write(g_pk[p].h, wstart, wb, (DWORD)wl);
        if (!wok) {   /* disk full or a write error: nothing of this piece goes into the index; the pack is cut back */
            DWORD err = GetLastError();
            pack_truncate(p, base);
            L("store: WRITE FAILED (error %lu%s) at pack %d: the slot(s) being saved will be dropped, the pool stays as it was", err, err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL ? ", disk full" : "", p);
            g_write_failed = 1;
            break;
        }
        AcquireSRWLockExclusive(&g_lock);
        g_pk[p].size = off;
        for (uint64_t i = 0; i < n; ++i) {
            if (!need[i]) continue;
            IEnt *e = ix_find(s->hash[k0 + i].lo, s->hash[k0 + i].hi);
            if (e && (e->cs & F_PENDING) && e->loc == (uint64_t)(s->mem + (k0 + i) * PAGE)) {
                if (!e->ref) { ix_remove(e); continue; }
                e->loc = ((uint64_t)p << 40) | offs[i]; e->cs = cs[i];
                g_pk[p].live += REC_HDR + (cs[i] & CS_MASK);
                ++written; wbytes += REC_HDR + (cs[i] & CS_MASK);
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    if (g_cur >= 0) FlushFileBuffers(g_pk[g_cur].h);
    L("store: background: %llu staged pages -> %llu written (%llu MB, ratio %.2f) in %.0f ms", s->n, written, wbytes >> 20,
      wbytes ? (double)written * PAGE / wbytes : 0.0, now_ms() - t0);
}
static void finish_stage(Stage *s) {
    char d[MAX_PATH];
    AcquireSRWLockExclusive(&g_lock);
    if (g_write_failed) {
        /* pages of this stage that did not reach a pack leave the index; slots waiting for it lose their pages */
        uint64_t gone = 0;
        for (uint64_t k = 0; k < s->n; ++k) {
            IEnt *e = ix_find(s->hash[k].lo, s->hash[k].hi);
            if (e && (e->cs & F_PENDING) && e->loc == (uint64_t)(s->mem + k * PAGE)) { e->ref = 0; ix_remove(e); ++gone; }
        }
        for (int i = 1; i <= STORE_MAX_SLOTS; ++i) {
            if (g_slot[i].state != 2 || g_slot[i].need_seq < s->seq) continue;
            TReg *reg; int nreg; H128 *tab; uint64_t n;
            uint8_t *b = read_table(i, "", &reg, &nreg, &tab, &n);
            if (b) { table_ref(tab, n, -1, NULL); free(b); }
            slot_dir(i, "", d); rm_tree(d);
            g_slot[i].state = 0; free(g_slot[i].meta); g_slot[i].meta = NULL;
            L("store: slot %d DROPPED: its pages could not be written (disk full?)", i);
            g_dropped_slot = i;
        }
        L("store: %llu pages of the failed save removed from the index", gone);
        g_write_failed = 0;
    }
    g_stages = s->next; if (!g_stages) g_stages_tail = &g_stages;
    g_stage_done = (LONG64)s->seq;
    big_free(s->mem, s->bytes); big_free(s->hash, s->n * sizeof(H128)); big_free(s, sizeof(Stage));
    if (!g_stages) ix_shrink();
    for (int i = 1; i <= STORE_MAX_SLOTS; ++i) {
        if (g_slot[i].state != 2 || g_slot[i].need_seq > (uint64_t)g_stage_done) continue;
        char *m = g_slot[i].meta, *q = m ? strstr(m, "\"complete\": false") : NULL;
        if (q) memcpy(q, "\"complete\": true ", 17);
        slot_dir(i, "", d);
        if (m) write_file(d, "metadata.json", m, strlen(m));
        g_slot[i].state = 1;
        L("store: slot %d complete on disk", i);
    }
    ReleaseSRWLockExclusive(&g_lock);
}
/* Rewrites the live records of a mostly dead pack into the current one. */
static void compact(int id) {
    AcquireSRWLockShared(&g_lock);
    uint64_t n = 0;
    for (uint64_t i = 0; i < g_ix_cap; ++i) if ((g_ix[i].lo || g_ix[i].hi) && !(g_ix[i].cs & F_PENDING) && (int)(g_ix[i].loc >> 40) == id) ++n;
    XEnt *x = (XEnt *)malloc((n + 1) * sizeof(XEnt)); uint64_t k = 0;
    for (uint64_t i = 0; x && i < g_ix_cap; ++i) {
        IEnt *e = &g_ix[i];
        if ((e->lo || e->hi) && !(e->cs & F_PENDING) && (int)(e->loc >> 40) == id) { x[k].lo = e->lo; x[k].hi = e->hi; x[k].cs = e->cs; x[k].off = e->loc & ((1ull << 40) - 1); ++k; }
    }
    ReleaseSRWLockShared(&g_lock);
    if (!x) return;
    uint8_t rec[PAGE + REC_HDR];
    uint64_t moved = 0;
    for (uint64_t i = 0; i < k; ++i) {
        uint32_t len = (uint32_t)REC_HDR + (x[i].cs & CS_MASK);
        if (!pread_(g_pk[id].h, x[i].off - REC_HDR, rec, len)) continue;
        int p = pack_current(); if (p < 0 || p == id) break;
        uint64_t off = g_pk[p].size;
        if (!pack_write(g_pk[p].h, off, rec, len)) { pack_truncate(p, off); L("store: compaction stopped: write error %lu (disk full?)", GetLastError()); break; }
        AcquireSRWLockExclusive(&g_lock);
        g_pk[p].size = off + len;
        IEnt *e = ix_find(x[i].lo, x[i].hi);
        if (e && !(e->cs & F_PENDING) && e->loc == (((uint64_t)id << 40) | x[i].off)) { e->loc = ((uint64_t)p << 40) | (off + REC_HDR); g_pk[p].live += len; ++moved; }
        ReleaseSRWLockExclusive(&g_lock);
    }
    if (g_cur >= 0) FlushFileBuffers(g_pk[g_cur].h);
    free(x);
    write_index();
    AcquireSRWLockExclusive(&g_lock);
    uint64_t left = 0;
    for (uint64_t i = 0; i < g_ix_cap; ++i) if ((g_ix[i].lo || g_ix[i].hi) && !(g_ix[i].cs & F_PENDING) && (int)(g_ix[i].loc >> 40) == id) ++left;
    uint64_t size = g_pk[id].size;
    if (!left) pack_delete(id);
    ReleaseSRWLockExclusive(&g_lock);
    L("store: compacted pack %d (%llu MB): %llu live pages moved%s", id, size >> 20, moved, left ? ", NOT deleted (pages left)" : ", deleted");
}
static void check_pool(void);
static DWORD WINAPI bg_thread(LPVOID a) {
    (void)a;
    check_pool();
    while (!g_bg_quit) {
        WaitForSingleObject(g_bg_ev, 1000);
        g_bg_busy = 1;
        int did = 0;
        for (;;) {
            AcquireSRWLockShared(&g_lock); Stage *s = g_stages; ReleaseSRWLockShared(&g_lock);
            if (!s || g_bg_quit) break;
            bg_stage(s); finish_stage(s); did = 1;
        }
        if (did) write_index();
        for (int id = 0; id < MAX_PACKS && !g_bg_quit; ++id) {
            AcquireSRWLockShared(&g_lock);
            int go = g_pk[id].exists && id != g_cur && g_pk[id].live * 2 < g_pk[id].size && !g_stages;
            ReleaseSRWLockShared(&g_lock);
            if (go) compact(id);
        }
        g_bg_busy = 0;
    }
    return 0;
}
int store_take_dropped(void) { return (int)InterlockedExchange(&g_dropped_slot, 0); }
void store_fail_writes(int on) { g_fail_writes = on; }
uint64_t store_free_bytes(void) {
    ULARGE_INTEGER fr; char p[MAX_PATH]; _snprintf_s(p, MAX_PATH, _TRUNCATE, "%s\\", g_dir);
    return GetDiskFreeSpaceExA(p, &fr, NULL, NULL) ? fr.QuadPart : ~0ull;
}
/* Pool check (background, low priority): every record a slot uses is read, decompressed and hashed.
 * A slot with a bad page is marked damaged: loads refuse it with the reason. */
static void check_pool(void) {
    double t0 = now_ms();
    uint64_t pages = 0, bad = 0;
    static uint8_t rec[PAGE + REC_HDR], pg[PAGE];
    ZSTD_DCtx *dc = ZSTD_createDCtx();
    for (int i = 1; i <= STORE_MAX_SLOTS && !g_bg_quit; ++i) {
        AcquireSRWLockShared(&g_lock); int st = g_slot[i].state; ReleaseSRWLockShared(&g_lock);
        if (st != 1) continue;
        TReg *reg; int nreg; H128 *tab; uint64_t n, sbad = 0;
        LONG gen0 = g_slot_gen[i];
        uint8_t *b = read_table(i, "", &reg, &nreg, &tab, &n);
        if (!b) continue;
        for (uint64_t k = 0; k < n && !g_bg_quit; ++k) {
            if (!tab[k].lo && !tab[k].hi) continue;
            AcquireSRWLockShared(&g_lock);   /* held while reading: compaction cannot move the record meanwhile */
            IEnt *e = ix_find(tab[k].lo, tab[k].hi);
            int ok = 0, p = -1; uint64_t off = 0; uint32_t cs = 0, raw = 0;
            if (!e || (e->cs & (F_PENDING | F_CHECKED))) ok = 2;   /* gone (slot saved again since) or done */
            else { p = (int)(e->loc >> 40); off = e->loc & ((1ull << 40) - 1); cs = e->cs & CS_MASK; raw = e->cs & F_RAW; }
            if (ok == 2) { ReleaseSRWLockShared(&g_lock); continue; }
            if (p >= 0 && g_pk[p].h && pread_(g_pk[p].h, off - REC_HDR, rec, (DWORD)(REC_HDR + (raw ? PAGE : cs)))) {
                const uint32_t *h32 = (const uint32_t *)rec; const uint64_t *h64 = (const uint64_t *)rec;
                if (h32[0] == REC_MAGIC && h64[1] == tab[k].lo && h64[2] == tab[k].hi) {
                    if (raw) memcpy(pg, rec + REC_HDR, PAGE);
                    size_t m = raw ? PAGE : ZSTD_decompressDCtx(dc, pg, PAGE, rec + REC_HDR, cs);
                    if (!ZSTD_isError(m) && m == PAGE) { H128 h = page_hash(pg); ok = h.lo == tab[k].lo && h.hi == tab[k].hi; }
                }
            }
            ReleaseSRWLockShared(&g_lock);
            ++pages;
            if (ok) { AcquireSRWLockExclusive(&g_lock); e = ix_find(tab[k].lo, tab[k].hi); if (e && !(e->cs & F_PENDING)) e->cs |= F_CHECKED; ReleaseSRWLockExclusive(&g_lock); }
            if (!ok) { ++bad; ++sbad; }
            if ((pages & 4095) == 0) Sleep(1);
        }
        free(b);
        if (g_slot_gen[i] != gen0) continue;   /* saved again during the check */
        g_check_bad[i] = (LONG)(sbad > 0x7fffffff ? 0x7fffffff : sbad);
        if (sbad) L("store: pool check: slot %d has %llu damaged pages: it will not load", i, sbad);
    }
    ZSTD_freeDCtx(dc);
    L("store: pool check: %llu stored pages of the slots read back in %.0f ms: %s", pages, now_ms() - t0, bad ? "DAMAGE FOUND" : "all intact");
}
int store_slot_damaged(int slot) { return slot >= 1 && slot <= STORE_MAX_SLOTS ? (int)g_check_bad[slot] : 0; }
void store_flush(void) {
    SetEvent(g_bg_ev);
    for (int i = 0; i < 6000; ++i) {
        AcquireSRWLockShared(&g_lock); int idle = !g_stages; ReleaseSRWLockShared(&g_lock);
        if (idle && !g_bg_busy) return;
        Sleep(10);
    }
}

/* ---- init ---- */
int store_init(const char *dir, int fast_threads, int bg_threads, LogFn log) {
    double t0 = now_ms();
    L = log;
    strncpy_s(g_dir, sizeof g_dir, dir, _TRUNCATE);
    CreateDirectoryA(g_dir, NULL);
    char p[MAX_PATH]; _snprintf_s(p, MAX_PATH, _TRUNCATE, "%s\\pool", g_dir); CreateDirectoryA(p, NULL);
    /* packs */
    WIN32_FIND_DATAA fd; _snprintf_s(p, MAX_PATH, _TRUNCATE, "%s\\pool\\pack_*.dat", g_dir);
    HANDLE h = FindFirstFileA(p, &fd);
    if (h != INVALID_HANDLE_VALUE) { do { int id = atoi(fd.cFileName + 5); if (id > 0 && id < MAX_PACKS) pack_open(id, 0); } while (FindNextFileA(h, &fd)); FindClose(h); }
    ix_grow(0);
    if (!read_index()) scan_packs();
    /* slots: finish interrupted swaps, count references */
    uint64_t dropped_slots = 0;
    for (int i = 1; i <= STORE_MAX_SLOTS; ++i) {
        char d[MAX_PATH], dn[MAX_PATH]; slot_dir(i, "", d); slot_dir(i, ".new", dn);
        DWORD an = GetFileAttributesA(dn), ad = GetFileAttributesA(d);
        if (an != INVALID_FILE_ATTRIBUTES) { if (ad == INVALID_FILE_ATTRIBUTES) MoveFileExA(dn, d, 0); else rm_tree(dn); }
        TReg *reg; int nreg; H128 *tab; uint64_t n, missing = 0;
        uint8_t *b = read_table(i, "", &reg, &nreg, &tab, &n);
        if (!b) { if (GetFileAttributesA(d) != INVALID_FILE_ATTRIBUTES) { L("store: slot %d unreadable, removed", i); rm_tree(d); ++dropped_slots; } continue; }
        for (uint64_t k = 0; k < n; ++k) if ((tab[k].lo || tab[k].hi) && !ix_find(tab[k].lo, tab[k].hi)) ++missing;
        if (missing) { L("store: slot %d lacks %llu pages in the pool (interrupted save), removed", i, missing); free(b); rm_tree(d); ++dropped_slots; continue; }
        table_ref(tab, n, +1, NULL);
        free(b);
        char mp[MAX_PATH]; uint64_t ml; _snprintf_s(mp, MAX_PATH, _TRUNCATE, "%s\\metadata.json", d);
        g_slot[i].meta = (char *)read_file(mp, &ml);
        if (g_slot[i].meta) { char *q = strstr(g_slot[i].meta, "\"complete\": false"); if (q) { memcpy(q, "\"complete\": true ", 17); write_file(d, "metadata.json", g_slot[i].meta, strlen(g_slot[i].meta)); } }
        g_slot[i].state = 1;
    }
    /* unreferenced pages are dead; live bytes per pack */
    uint64_t dead = 0;
    for (uint64_t i = 0; i < g_ix_cap; ) {
        IEnt *e = &g_ix[i];
        if ((e->lo || e->hi) && !e->ref) { ix_remove(e); ++dead; continue; } /* re-check slot i after the shift */
        ++i;
    }
    for (uint64_t i = 0; i < g_ix_cap; ++i) if (g_ix[i].lo || g_ix[i].hi) g_pk[g_ix[i].loc >> 40].live += REC_HDR + (g_ix[i].cs & CS_MASK);
    if (fast_threads > MAX_THR) fast_threads = MAX_THR;
    if (bg_threads > MAX_THR) bg_threads = MAX_THR;
    for (int t = 0; t < MAX_THR; ++t) {
        uint8_t tmp[PAGE] = {1}, o[PAGE * 2]; /* size the contexts now, not while the game is paused */
        size_t c = 0;
        if (t < bg_threads) { g_cctx[t] = ZSTD_createCCtx(); c = ZSTD_compressCCtx(g_cctx[t], o, sizeof o, tmp, PAGE, 1); }
        if (t < fast_threads) {
            g_dctx[t] = ZSTD_createDCtx();
            g_rdbuf[t] = (uint8_t *)VirtualAlloc(NULL, 2 * PAGE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!c) { ZSTD_CCtx *cc = ZSTD_createCCtx(); c = ZSTD_compressCCtx(cc, o, sizeof o, tmp, PAGE, 1); ZSTD_freeCCtx(cc); }
            ZSTD_decompressDCtx(g_dctx[t], tmp, PAGE, o, c);
        }
    }
    pool_start(&g_fast, 0, fast_threads, THREAD_PRIORITY_HIGHEST);
    pool_start(&g_bgp, 1, bg_threads, THREAD_PRIORITY_BELOW_NORMAL);
    g_bg_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_bg_th = CreateThread(NULL, 1 << 20, bg_thread, NULL, 0, NULL);
    SetThreadPriority(g_bg_th, THREAD_PRIORITY_BELOW_NORMAL);
    L("store: %s: %llu pages in the pool (%llu unreferenced dropped), %llu slots dropped, init %.0f ms", g_dir, g_ix_n, dead, dropped_slots, now_ms() - t0);
    if (dead) write_index();
    store_status();
    return 1;
}
void store_shutdown(void) {
    store_flush();
    g_bg_quit = 1; SetEvent(g_bg_ev);
    WaitForSingleObject(g_bg_th, 10000); CloseHandle(g_bg_th);
    pool_stop(&g_fast); pool_stop(&g_bgp);
    write_index();
    for (int i = 0; i < MAX_PACKS; ++i) if (g_pk[i].h) { CloseHandle(g_pk[i].h); g_pk[i].h = NULL; }
    for (int t = 0; t < MAX_THR; ++t) { ZSTD_freeDCtx(g_dctx[t]); ZSTD_freeCCtx(g_cctx[t]); VirtualFree(g_rdbuf[t], 0, MEM_RELEASE); }
    big_free(g_ix, g_ix_cap * sizeof(IEnt)); g_ix = NULL; g_ix_cap = g_ix_n = 0;
    for (int i = 1; i <= STORE_MAX_SLOTS; ++i) { free(g_slot[i].meta); g_slot[i].meta = NULL; }
}
