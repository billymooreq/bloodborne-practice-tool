/* bbstate.dll: savestates for bb-probe.exe 1.9.5 (bbport). Loaded by launch.py before the game's first
 * instruction (early mode: vsync handles, slots load in another run of the game = L3), or injected into a
 * running game with inject.py (L1 only: the same process).
 *
 * L3 (another process): host references in guest memory are translated (XREF ranges), thread-locals and
 * the guest frames of every thread's stack are transplanted from the slot (unwind to the first guest frame,
 * guest frame-pointer chain), the vsync table replaces this process's, all GPU caches are dropped.
 *
 * - Checks the exe's sha256; any other build: logs and does nothing.
 * - Hooks guest import slots (thunks.asm): sceGnmSubmitDone stops the guest render thread at a frame
 *   boundary on request; mutex/cond/attr create+destroy are tracked so host objects a snapshot may
 *   still reference are never freed.
 * - Consistent point: render thread stopped in the hook, GPU idle, every other guest thread
 *   suspended inside an ntdll wait. Otherwise the frame is released and the next one is tried.
 * - Save (L1): guest memory (VMAs through backing_base + RW pages of the eboot image) goes to the
 *   slot store on disk (store.c: page pool with dedup, zstd). Thread stacks/TLS/contexts and the
 *   host object list are written next to it for later research (not restored).
 * - Load (L1): only in the same process and the same location instance (same VMAs and guest
 *   threads): pages that differ from the slot are written back, GPU caches invalidated.
 * - Watcher: player HP, PlayerIns, guest thread set and VMAs are polled every 10 ms; changes are
 *   logged with timestamps. Optional auto-load of the active slot when the player dies.
 * Keys and options: bbstate.ini next to this file. Commands: lines in cmd.txt (see handle_command).
 *
 * Nothing that takes the process heap or the loader lock runs while guest threads are suspended. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <intrin.h>
#include "rva.h"
#include "store.h"
#include "vsync.h"
#include "bbs_core.h"
#include "meta.h"
#include "keys.h"
#include "ov.h"
#include "practice.h"
#pragma comment(lib, "synchronization.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")

#define PAGE 4096ull
#define MAX_THREADS 256
#define MAX_IMG 2048
#define MAX_VMAS 1024
#define MAX_MODULES 512
#define FORMAT_VERSION 13  /* 8: DLL addresses only through the gate (slots not tied to the DLL build); 9: semaphores in vsync; 10: guest clocks; 11: GPU compute queue positions; 12: canonical stack depth; 13: dead stack window */

typedef struct { uint64_t start, end; int32_t kind, prot, type, pad; uint64_t phys; } Vma; /* 40 B */

/* ---- globals ---- */
static HMODULE g_self;
static uint8_t *g_base;          /* bb-probe.exe */
static char g_dir[MAX_PATH];     /* data folder: <game>\practice-tool (pdh.dll proxy or when it exists), BBSTATE_DIR, else the DLL folder */
static int g_proxy;              /* loaded as pdh.dll next to bb-probe.exe */
static int g_supported;          /* bb-probe.exe is bbport 1.9.5 (sha256 checked in DllMain) */
static char g_exe_sha[65];
static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_log_cs;
static DWORD g_worker_tid;
static LARGE_INTEGER g_qpf;
static uint64_t g_proc_start;    /* process creation FILETIME */
static uint8_t *g_gate;          /* entries the guest calls instead of DLL functions (gate_init) */

uint64_t bbs_orig[3];
extern void bbs_hook0(void), bbs_hook1(void), bbs_hook2(void);
static void (*const k_hooks[3])(void) = {bbs_hook0, bbs_hook1, bbs_hook2};
static const uint64_t k_slots[3] = {SLOT_SUBMITDONE, SLOT_SUBMITANDFLIP, SLOT_VIDEOOUTSUBMITFLIP};
static const uint64_t k_fn_rva[3] = {RVA_FN_SUBMITDONE, RVA_FN_SUBMITANDFLIP, RVA_FN_VIDEOOUTSUBMITFLIP};
static const char *const k_names[3] = {"sceGnmSubmitDone", "sceGnmSubmitAndFlipCommandBuffers", "sceVideoOutSubmitFlip"};
static int g_hooked;

static volatile LONG64 g_count[3];
static volatile LONG g_last_tid[3];
/* pause handshake: 0 idle, 1 requested, 2 render thread stopped in the hook */
static volatile LONG g_state;
static volatile LONG g_pause_kind = 0;
static HANDLE g_ev_paused, g_ev_resume;
static uint64_t g_pause_frame, g_pause_stackbase;
static DWORD g_pause_tid;

/* ---- options (bbstate.ini) ---- */
typedef HotKey Key;
static struct {
    Key save, load, next, prev;
    int active, autoload, beeps, beep_volume, fast_threads, bg_threads, watch, l3_tries, l1_tries, min_free_mb;
    char slots_dir[MAX_PATH], save_files[MAX_PATH], ini[MAX_PATH];
} O;

#ifdef BBS_SHIFT_TEST   /* rebuild test: moves this DLL's code and data (slots of the normal build must still load) */
static volatile uint8_t g_shift_pad[200000] = {1};
static __declspec(noinline) int shift_code(int x) {
    for (int i = 0; i < 64; ++i) { x = x * 1103515245 + 12345; g_shift_pad[(x >> 8) % sizeof g_shift_pad] ^= (uint8_t)x; if (x % 7 == 3) x ^= g_shift_pad[i * 97]; }
    switch (x & 15) { case 0: x += 3; break; case 1: x *= 5; break; case 2: x ^= 0x55; break; case 3: x -= 9; break; case 4: x += g_shift_pad[5]; break;
                      case 5: x <<= 1; break; case 6: x >>= 2; break; case 7: x |= 0x100; break; default: x &= 0xffff; }
    return x;
}
#endif
/* ---- logging ---- */
static void blog(const char *fmt, ...) {
    static char buf[8192];
    SYSTEMTIME st;
    va_list ap;
    int n, m;
    DWORD w;
    if (g_log == INVALID_HANDLE_VALUE) return;
    EnterCriticalSection(&g_log_cs);
    GetLocalTime(&st);
    n = _snprintf_s(buf, sizeof buf, _TRUNCATE, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    m = _vsnprintf_s(buf + n, sizeof buf - n - 3, _TRUNCATE, fmt, ap);
    va_end(ap);
    n = m < 0 ? (int)strlen(buf) : n + m;
    buf[n++] = '\r'; buf[n++] = '\n';
    WriteFile(g_log, buf, (DWORD)n, &w, NULL);
    LeaveCriticalSection(&g_log_cs);
}
static double now_ms(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart * 1000.0 / g_qpf.QuadPart; }

/* ---- sounds: short sine tones, played asynchronously from memory ---- */
enum { SND_SAVED, SND_LOADED, SND_REFUSED, SND_SLOT1, SND_COUNT = SND_SLOT1 + 10 };
static uint8_t *g_snd[SND_COUNT];
typedef struct { int hz, ms, gap; } Tone;
static uint8_t *make_wav(const Tone *t, int n, int volume) {
    const int rate = 22050;
    int samples = 0;
    for (int i = 0; i < n; ++i) samples += (t[i].ms + t[i].gap) * rate / 1000;
    uint32_t data = samples * 2;
    uint8_t *w = (uint8_t *)calloc(1, 44 + data);
    if (!w) return NULL;
    uint32_t h32[] = {0x46464952, 36 + data, 0x45564157, 0x20746d66, 16};
    memcpy(w, h32, 20);
    uint16_t fmt[] = {1, 1}; memcpy(w + 20, fmt, 4);
    uint32_t r[] = {rate, rate * 2}; memcpy(w + 24, r, 8);
    uint16_t b[] = {2, 16}; memcpy(w + 32, b, 4);
    uint32_t d[] = {0x61746164, data}; memcpy(w + 36, d, 8);
    int16_t *s = (int16_t *)(w + 44);
    double amp = 32767.0 * volume / 100.0;
    for (int i = 0, k = 0; i < n; ++i) {
        int len = t[i].ms * rate / 1000, fade = rate / 200;
        for (int j = 0; j < len; ++j, ++k) {
            double env = j < fade ? (double)j / fade : len - j < fade ? (double)(len - j) / fade : 1.0;
            s[k] = (int16_t)(amp * env * sin(2 * 3.14159265358979 * t[i].hz * j / rate));
        }
        k += t[i].gap * rate / 1000;
    }
    return w;
}
static void init_sounds(void) {
    Tone saved[] = {{880, 70, 20}, {1320, 90, 0}}, loaded[] = {{1320, 70, 20}, {880, 90, 0}}, refused[] = {{220, 280, 0}};
    g_snd[SND_SAVED] = make_wav(saved, 2, O.beep_volume);
    g_snd[SND_LOADED] = make_wav(loaded, 2, O.beep_volume);
    g_snd[SND_REFUSED] = make_wav(refused, 1, O.beep_volume);
    for (int n = 1; n <= 10; ++n) { /* slot N: one long tone per 5, one short per remaining 1 */
        Tone t[8]; int k = 0;
        for (int i = 0; i < n / 5; ++i) t[k++] = (Tone){660, 220, 80};
        for (int i = 0; i < n % 5; ++i) t[k++] = (Tone){990, 55, 75};
        g_snd[SND_SLOT1 + n - 1] = make_wav(t, k, O.beep_volume);
    }
}
static void play(int s) { if (O.beeps && g_snd[s]) PlaySoundA((LPCSTR)g_snd[s], NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT); }

/* ---- address description (no heap, no loader lock) ---- */
typedef struct { uint64_t base, size; char name[48]; } Module;
static Module g_mods[MAX_MODULES];
static int g_nmods;
typedef struct { uint32_t rva; const char *name; } Export;
static Export *g_ntx; static int g_nntx;
static uint64_t g_ntdll;

static int cmp_export(const void *a, const void *b) {
    uint32_t x = ((const Export *)a)->rva, y = ((const Export *)b)->rva;
    return x < y ? -1 : x > y;
}
static void init_modules(void) {
    HMODULE mods[MAX_MODULES];
    DWORD need = 0;
    HANDLE p = GetCurrentProcess();
    if (K32EnumProcessModules(p, mods, sizeof mods, &need)) {
        int n = (int)(need / sizeof(HMODULE));
        for (int i = 0; i < n && i < MAX_MODULES; ++i) {
            MODULEINFO mi;
            if (!K32GetModuleInformation(p, mods[i], &mi, sizeof mi)) continue;
            g_mods[g_nmods].base = (uint64_t)mi.lpBaseOfDll;
            g_mods[g_nmods].size = mi.SizeOfImage;
            K32GetModuleBaseNameA(p, mods[i], g_mods[g_nmods].name, sizeof g_mods[g_nmods].name);
            ++g_nmods;
        }
    }
    if (g_gate && g_nmods < MAX_MODULES) {   /* the gate is a module of its own: a slot maps it by name, not by DLL build */
        g_mods[g_nmods].base = (uint64_t)g_gate; g_mods[g_nmods].size = 4096; strcpy_s(g_mods[g_nmods].name, sizeof g_mods[g_nmods].name, "bbgate"); ++g_nmods;
    }
    uint8_t *nt = (uint8_t *)GetModuleHandleA("ntdll.dll");
    g_ntdll = (uint64_t)nt;
    IMAGE_NT_HEADERS64 *h = (IMAGE_NT_HEADERS64 *)(nt + ((IMAGE_DOS_HEADER *)nt)->e_lfanew);
    IMAGE_EXPORT_DIRECTORY *ed = (IMAGE_EXPORT_DIRECTORY *)(nt + h->OptionalHeader.DataDirectory[0].VirtualAddress);
    uint32_t *names = (uint32_t *)(nt + ed->AddressOfNames), *funcs = (uint32_t *)(nt + ed->AddressOfFunctions);
    uint16_t *ords = (uint16_t *)(nt + ed->AddressOfNameOrdinals);
    g_ntx = (Export *)malloc(sizeof(Export) * ed->NumberOfNames);
    for (uint32_t i = 0; i < ed->NumberOfNames; ++i) {
        g_ntx[g_nntx].rva = funcs[ords[i]];
        g_ntx[g_nntx].name = (const char *)(nt + names[i]);
        ++g_nntx;
    }
    qsort(g_ntx, g_nntx, sizeof(Export), cmp_export);
}
static const char *ntdll_func(uint64_t a) {
    if (!g_nntx || a < g_ntdll) return NULL;
    uint64_t rva = a - g_ntdll;
    int lo = 0, hi = g_nntx - 1, best = -1;
    while (lo <= hi) { int mid = (lo + hi) / 2; if (g_ntx[mid].rva <= rva) { best = mid; lo = mid + 1; } else hi = mid - 1; }
    if (best < 0 || rva - g_ntx[best].rva > 0x400) return NULL;
    return g_ntx[best].name;
}
static uint64_t low_next(void) { return *(uint64_t *)(g_base + RVA_LOW_NEXT); }
static void describe(uint64_t a, char *out, size_t size) {
    if (a >= 0x800000000ull && a < low_next()) { _snprintf_s(out, size, _TRUNCATE, "guest+0x%llx", a - 0x800000000ull); return; }
    for (int i = 0; i < g_nmods; ++i)
        if (a >= g_mods[i].base && a < g_mods[i].base + g_mods[i].size) {
            const char *f = g_mods[i].base == g_ntdll ? ntdll_func(a) : NULL;
            _snprintf_s(out, size, _TRUNCATE, "%s+0x%llx%s%s", g_mods[i].name, a - g_mods[i].base, f ? " " : "", f ? f : "");
            return;
        }
    _snprintf_s(out, size, _TRUNCATE, "0x%llx", a);
}
static int is_code_addr(uint64_t v) {
    if (v >= 0x800000000ull && v < 0x805400000ull) return 1; /* eboot text */
    for (int i = 0; i < g_nmods; ++i) if (v >= g_mods[i].base + 0x1000 && v < g_mods[i].base + g_mods[i].size) return 1;
    return 0;
}
/* A thread is "quiet" when it is suspended inside one of these ntdll waits. */
static const char *wait_name(uint64_t rip) {
    static const char *const ok[] = {"WaitForSingleObject", "WaitForMultipleObjects", "WaitForMultipleObjects32",
        "WaitForAlertByThreadId", "DelayExecution", "RemoveIoCompletion", "RemoveIoCompletionEx",
        "WaitForWorkViaWorkerFactory", "YieldExecution", "SignalAndWaitForSingleObject", "WaitForKeyedEvent",
        "ReplyWaitReceivePort", "ReplyWaitReceivePortEx", "AlpcSendWaitReceivePort"};
    const char *f = ntdll_func(rip);
    if (!f || !((f[0] == 'N' && f[1] == 't') || (f[0] == 'Z' && f[1] == 'w'))) return NULL;
    for (size_t i = 0; i < sizeof ok / sizeof *ok; ++i) if (!strcmp(f + 2, ok[i])) return f;
    return NULL;
}

/* ---- sha256 of the exe ---- */
static int exe_hash_ok(void) {
    char path[MAX_PATH];
    uint8_t digest[32];
    BCRYPT_ALG_HANDLE alg = NULL; BCRYPT_HASH_HANDLE hh = NULL;
    GetModuleFileNameA(NULL, path, sizeof path);
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { blog("cannot open %s", path); return 0; }
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) || BCryptCreateHash(alg, &hh, NULL, 0, NULL, 0, 0)) {
        CloseHandle(f); blog("bcrypt failed"); return 0;
    }
    static uint8_t chunk[1 << 20];
    DWORD got;
    while (ReadFile(f, chunk, sizeof chunk, &got, NULL) && got) BCryptHashData(hh, chunk, got, 0);
    BCryptFinishHash(hh, digest, 32, 0);
    BCryptDestroyHash(hh); BCryptCloseAlgorithmProvider(alg, 0); CloseHandle(f);
    for (int i = 0; i < 32; ++i) sprintf_s(g_exe_sha + 2 * i, 3, "%02x", digest[i]);
    blog("exe %s sha256 %s", path, g_exe_sha);
    return !strcmp(g_exe_sha, BBPROBE_195_SHA256);
}

/* ---- early mode (launch.py): loaded by an APC before the exe's entry point ----
 * The import resolver runtime_resolve(name, is_data) is patched (5-byte jmp to a stub near the exe):
 * mutex/cond/attr/rwlock NIDs get the vsync wrappers, everything else goes to the original through a
 * trampoline. The guest stack canary becomes a constant: restored fiber stacks keep the canary of the
 * process that saved them. */
static int g_early;      /* DLL was loaded before the game image existed */
static int g_virt;       /* vsync handed out (BBSTATE_EARLY=1) */
static uint8_t *g_tramp;
#define FIXED_CANARY 0x2F8B5A61C3D49E00ull
static volatile LONG g_resolve_calls;
static uint64_t gate_of(const void *fn);
/* ---- guest clocks ----
 * The guest reads the raw TSC (sceKernelReadTsc) and the host's monotonic clock (clock ids MONOTONIC/
 * UPTIME/network) besides the process clock: those restart at 0 with the PC, so a slot from before a
 * restart would see time go backwards. The resolver hands out these wrappers: host value + offset, and a
 * load moves the offsets forward (never back) so the slot's clocks continue. */
extern void vc_tsc(void), vc_kclock(void), vc_pclock(void);
extern int64_t sysv_call2(void *fn, uint64_t a, uint64_t b);
static volatile int64_t g_tsc_off, g_mono_off_ns;
static int g_clockfix = 1;   /* command "clockfix 0": loads do not move the clocks (to show the problem) */
typedef struct { int64_t sec, nsec; } GTs;
uint64_t vc_tsc_c(void) { return __rdtsc() + (uint64_t)g_tsc_off; }
static int mono_clock(uint32_t id) { return id == 4 || id == 5 || id == 7 || id == 8 || id == 11 || id == 12 || (id >= 16 && id <= 19); }
static void ts_add(GTs *t, int64_t ns) { int64_t v = t->sec * 1000000000ll + t->nsec + ns; t->sec = v / 1000000000ll; t->nsec = v % 1000000000ll; }
int32_t vc_kclock_c(uint32_t id, GTs *ts) {
    int32_t r = (int32_t)sysv_call2(g_base + RVA_KERNEL_CLOCK_GETTIME, id, (uint64_t)ts);
    if (!r && ts && g_mono_off_ns && mono_clock(id)) ts_add(ts, g_mono_off_ns);
    return r;
}
int32_t vc_pclock_c(uint32_t id, GTs *ts) {
    int32_t r = (int32_t)sysv_call2(g_base + RVA_POSIX_CLOCK_GETTIME, id, (uint64_t)ts);
    if (!r && ts && g_mono_off_ns && mono_clock(id)) ts_add(ts, g_mono_off_ns);
    return r;
}
static int64_t mono_guest_ns(void) {   /* the guest's monotonic clock now */
    GTs t = {0, 0}; sysv_call2(g_base + RVA_POSIX_CLOCK_GETTIME, 4, (uint64_t)&t);
    return t.sec * 1000000000ll + t.nsec + g_mono_off_ns;
}
static uint64_t g_tsc_hz;
static void measure_tsc(void) {
    LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a); uint64_t t0 = __rdtsc();
    Sleep(50); QueryPerformanceCounter(&b); uint64_t t1 = __rdtsc();
    g_tsc_hz = (uint64_t)((double)(t1 - t0) * f.QuadPart / (double)(b.QuadPart - a.QuadPart));
}
typedef uint64_t (*ResolveFn)(const char *, int);
static uint64_t hook_resolve(const char *name, int is_data) {
    InterlockedIncrement(&g_resolve_calls);
    uint64_t *canary = (uint64_t *)(g_base + RVA_STACK_CANARY);
    if (*canary != FIXED_CANARY) *canary = FIXED_CANARY;
    if (!is_data && g_virt) { uint64_t w = vs_lookup(name); if (w) return gate_of((const void *)w); }
    uint64_t r = ((ResolveFn)g_tramp)(name, is_data);
    if (!is_data && g_virt && r) {
        if (r == (uint64_t)g_base + RVA_READ_TSC) return gate_of((const void *)vc_tsc);
        if (r == (uint64_t)g_base + RVA_KERNEL_CLOCK_GETTIME) return gate_of((const void *)vc_kclock);
        if (r == (uint64_t)g_base + RVA_POSIX_CLOCK_GETTIME) return gate_of((const void *)vc_pclock);
    }
    return r;
}
static uint8_t *alloc_near(uint8_t *base, SIZE_T size) {
    for (uint64_t a = ((uint64_t)base - 0x10000) & ~0xffffull; a > (uint64_t)base - 0x7ff00000ull; a -= 0x10000) {
        void *p = VirtualAlloc((void *)a, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) return (uint8_t *)p;
    }
    return NULL;
}
static const char *install_resolve_hook(void) {
    static const uint8_t prologue[11] = {0x55, 0x56, 0x48, 0x83, 0xec, 0x28, 0x48, 0x8d, 0x6c, 0x24, 0x20};
    uint8_t *fn = g_base + RVA_RUNTIME_RESOLVE;
    if (memcmp(fn, prologue, sizeof prologue)) return "runtime_resolve prologue differs (not bb-probe 1.9.5?)";
    uint8_t *stub = alloc_near(g_base, 4096);
    if (!stub) return "no memory near the exe";
    stub[0] = 0xff; stub[1] = 0x25; memset(stub + 2, 0, 4); *(uint64_t *)(stub + 6) = (uint64_t)hook_resolve;   /* jmp [rip]: hook */
    g_tramp = stub + 64;                                                                                            /* original prologue, back to fn+6 */
    memcpy(g_tramp, fn, 6);
    g_tramp[6] = 0xff; g_tramp[7] = 0x25; memset(g_tramp + 8, 0, 4); *(uint64_t *)(g_tramp + 12) = (uint64_t)(fn + 6);
    DWORD old;
    if (!VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old)) return "VirtualProtect failed";
    int32_t rel = (int32_t)((int64_t)stub - (int64_t)(fn + 5));
    uint8_t jmp[5] = {0xe9}; memcpy(jmp + 1, &rel, 4);
    memcpy(fn, jmp, 5);
    VirtualProtect(fn, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
    return NULL;
}
/* Canonical stack depth (thunks.asm canon_entry / canon_start): the exe entry (main thread) and the
 * runtime's host_start (every guest thread) begin with the stack pointer at a fixed distance below the
 * top, so a game frame sits at the same distance from the top in every run and a cross-process load
 * moves stack addresses by whole 64 KB steps (the top's own alignment): values that only look like
 * stack addresses (a flag byte over a stale address) keep their low bits. */
uint64_t canon_main_depth = 0x8000, canon_thread_depth = 0x3000, canon_entry_tramp, canon_start_tramp;
volatile LONG canon_done, canon_missed;
static int g_canon;
extern void canon_entry(void), canon_start(void);
static void jmp_abs(uint8_t *at, uint64_t to) { at[0] = 0xff; at[1] = 0x25; memset(at + 2, 0, 4); *(uint64_t *)(at + 6) = to; }
static int patch_jmp5(uint8_t *fn, uint8_t *to) {
    DWORD old;
    if (!VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    int32_t rel = (int32_t)((int64_t)to - (int64_t)(fn + 5));
    uint8_t jmp[5] = {0xe9}; memcpy(jmp + 1, &rel, 4);
    memcpy(fn, jmp, 5);
    VirtualProtect(fn, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
    return 1;
}
/* Task 6.1: fixed stack addresses (early mode). Windows puts every thread's stack at another address in
 * every run (and the canonical depth only fixes the distance from the top), so a cross-process load had to
 * translate every value that looks like a stack address, numbers included (the process main thread's
 * stack sat at 12-32 MB: plain integers hit it). Here the main thread and every guest thread run on a stack
 * of an arena reserved at a fixed address before the game starts: the main thread at its start, a guest
 * thread in a slot chosen by its name (hash, linear probing; the same names get the same slots in every
 * run), the slot's top fixed, the whole stack committed (no guard page growth), TEB StackBase/StackLimit
 * set (exception dispatch and unwinding check them; DeallocationStack stays: the system frees the
 * thread's own stack at exit). Stack addresses are then the same in every run: nothing to translate.
 * BBSTATE_FIXSTACK=0 turns it off (canonical depth only). */
#define FS_ARENA      0x200000000ull
#define FS_MAIN_SIZE  (32ull << 20)
#define FS_SLOT_SIZE  (2ull << 20)
#define FS_NSLOTS     1024
static int g_fs_on; static DWORD g_main_tid; static SRWLOCK g_fs_lock = SRWLOCK_INIT;
static struct { DWORD tid; int used; uint64_t lo, top, old_base, old_limit; char name[16]; } g_fs[FS_NSLOTS + 1];   /* [FS_NSLOTS] = main thread */
static volatile LONG g_fs_count, g_fs_fail, g_fs_probe;
static const char *fs_reserve(void) {
    char v[8] = "";
    if (GetEnvironmentVariableA("BBSTATE_FIXSTACK", v, sizeof v) && v[0] == '0') return "off (BBSTATE_FIXSTACK=0)";
    void *p = VirtualAlloc((void *)FS_ARENA, FS_MAIN_SIZE + FS_NSLOTS * FS_SLOT_SIZE, MEM_RESERVE, PAGE_READWRITE);
    if (p != (void *)FS_ARENA) { if (p) VirtualFree(p, 0, MEM_RELEASE); return "arena address not free"; }
    g_fs_on = 1;
    return NULL;
}
static uint64_t fs_slot_top(int s) { return s == FS_NSLOTS ? FS_ARENA + FS_MAIN_SIZE : FS_ARENA + FS_MAIN_SIZE + (uint64_t)(s + 1) * FS_SLOT_SIZE; }
uint64_t fs_enter(uint8_t *gt) {
    if (!g_fs_on) return 0;
    int is_main = GetCurrentThreadId() == g_main_tid;
    uint64_t base = __readgsqword(0x08), dealloc = *(uint64_t *)(__readgsqword(0x30) + 0x1478);
    uint64_t size = (base - dealloc + 0xffff) & ~0xffffull, cap = is_main ? FS_MAIN_SIZE : FS_SLOT_SIZE - 0x10000;
    if (size < (256ull << 10)) size = 256ull << 10;
    if (size > cap) size = cap;
    char name[16] = "main";
    if (!is_main) { __try { memcpy(name, gt + GT_NAME, 15); name[15] = 0; } __except (EXCEPTION_EXECUTE_HANDLER) { name[0] = 0; } }
    AcquireSRWLockExclusive(&g_fs_lock);
    int s = FS_NSLOTS, probes = 0;
    if (!is_main) {
        uint32_t h = 2166136261u; for (int i = 0; i < 15 && name[i]; ++i) { h ^= (uint8_t)name[i]; h *= 16777619u; }
        for (s = h % FS_NSLOTS; g_fs[s].used && probes < FS_NSLOTS; s = (s + 1) % FS_NSLOTS) ++probes;
        if (probes == FS_NSLOTS) { ReleaseSRWLockExclusive(&g_fs_lock); InterlockedIncrement(&g_fs_fail); return 0; }
    } else if (g_fs[s].used) { ReleaseSRWLockExclusive(&g_fs_lock); return 0; }
    uint64_t top = fs_slot_top(s), lo = top - size;
    if (!VirtualAlloc((void *)lo, size, MEM_COMMIT, PAGE_READWRITE)) { ReleaseSRWLockExclusive(&g_fs_lock); InterlockedIncrement(&g_fs_fail); return 0; }
    g_fs[s].used = 1; g_fs[s].tid = GetCurrentThreadId(); g_fs[s].lo = lo; g_fs[s].top = top;
    g_fs[s].old_base = base; g_fs[s].old_limit = __readgsqword(0x10); memcpy(g_fs[s].name, name, 16);
    ReleaseSRWLockExclusive(&g_fs_lock);
    InterlockedIncrement(&g_fs_count); InterlockedAdd(&g_fs_probe, probes);
    __writegsqword(0x10, lo); __writegsqword(0x08, top);
    return top - (is_main ? canon_main_depth : canon_thread_depth);
}
void fs_leave(void) {
    if (!g_fs_on) return;
    DWORD me = GetCurrentThreadId();
    AcquireSRWLockExclusive(&g_fs_lock);
    for (int s = 0; s <= FS_NSLOTS; ++s) {
        if (!g_fs[s].used || g_fs[s].tid != me) continue;
        __writegsqword(0x08, g_fs[s].old_base); __writegsqword(0x10, g_fs[s].old_limit);
        VirtualFree((void *)g_fs[s].lo, g_fs[s].top - g_fs[s].lo, MEM_DECOMMIT);
        g_fs[s].used = 0; g_fs[s].tid = 0;
        break;
    }
    ReleaseSRWLockExclusive(&g_fs_lock);
}
static const char *install_canon(void) {
    uint8_t *entry = g_base + RVA_EXE_ENTRY, *hs = g_base + RVA_HOST_START;
    static const uint8_t e_mid[7] = {0xc7, 0x00, 0x01, 0x00, 0x00, 0x00, 0xe9};   /* mov rax, [rip+d]; mov dword [rax], 1; jmp __tmainCRTStartup */
    static const uint8_t h_pro[11] = {0x55, 0x56, 0x57, 0x53, 0x48, 0x81, 0xec, 0xd8, 0x00, 0x00, 0x00};
    if (entry[0] != 0x48 || entry[1] != 0x8b || entry[2] != 0x05 || memcmp(entry + 7, e_mid, 7)) return "exe entry point differs";
    if (memcmp(hs, h_pro, sizeof h_pro)) return "host_start prologue differs";
    uint8_t *page = alloc_near(g_base, 4096);
    if (!page) return "no memory near the exe";
    uint64_t app_type = (uint64_t)(entry + 7) + *(int32_t *)(entry + 3), tmain = (uint64_t)(entry + 18) + *(int32_t *)(entry + 14);
    jmp_abs(page, (uint64_t)canon_entry); jmp_abs(page + 16, (uint64_t)canon_start);
    uint8_t *t = page + 64;   /* the entry's own instructions: movabs rax, &app_type; mov rax, [rax]; mov dword [rax], 1; jmp tmain */
    t[0] = 0x48; t[1] = 0xb8; memcpy(t + 2, &app_type, 8); t[10] = 0x48; t[11] = 0x8b; t[12] = 0x00;
    t[13] = 0xc7; t[14] = 0x00; t[15] = 1; t[16] = t[17] = t[18] = 0; jmp_abs(t + 19, tmain);
    canon_entry_tramp = (uint64_t)t;
    memcpy(page + 128, hs, sizeof h_pro); jmp_abs(page + 128 + sizeof h_pro, (uint64_t)hs + sizeof h_pro);
    canon_start_tramp = (uint64_t)(page + 128);
    FlushInstructionCache(GetCurrentProcess(), page, 4096);
    if (!patch_jmp5(entry, page) || !patch_jmp5(hs, page + 16)) return "VirtualProtect failed";
    g_canon = 1;
    return NULL;
}
static void gate_init(void);
/* Early mode: the game's stdout/stderr (runtime messages, the port's fault report with the call chain) go
 * to gamelogs\game_<time>_<pid>.log: the exe's C runtime picks the standard handles up at its start.
 * The last 20 are kept. */
static void game_output_to_file(void) {
    char d[MAX_PATH], p[MAX_PATH], pat[MAX_PATH];
    _snprintf_s(d, sizeof d, _TRUNCATE, "%s\\logs", g_dir);
    CreateDirectoryA(d, NULL);
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "%s\\game_*.log", d);
    static char names[256][64]; int n = 0;
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) { do { if (n < 256) strncpy_s(names[n++], 64, fd.cFileName, _TRUNCATE); } while (FindNextFileA(h, &fd)); FindClose(h); }
    qsort(names, n, 64, (int (*)(const void *, const void *))strcmp);
    for (int i = 0; i + 19 < n; ++i) { _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\%s", d, names[i]); DeleteFileA(p); }
    SYSTEMTIME st; GetLocalTime(&st);
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\game_%04d%02d%02d_%02d%02d%02d_%lu.log", d, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE f = CreateFileA(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    SetStdHandle(STD_OUTPUT_HANDLE, f); SetStdHandle(STD_ERROR_HANDLE, f);
    /* the exe's C runtime (ucrtbase) has set up its descriptors already: point 1 and 2 at the file */
    HMODULE crt = GetModuleHandleA("ucrtbase.dll");
    int (*open_osf)(intptr_t, int) = crt ? (int (*)(intptr_t, int))GetProcAddress(crt, "_open_osfhandle") : NULL;
    int (*dup2_)(int, int) = crt ? (int (*)(int, int))GetProcAddress(crt, "_dup2") : NULL;
    if (open_osf && dup2_) {
        int fd = open_osf((intptr_t)f, 0x0008 /* _O_APPEND */);
        if (fd >= 0) { dup2_(fd, 1); dup2_(fd, 2); }
    }
}
static void early_attach(void) {
    char v[8] = "";
    GetEnvironmentVariableA("BBSTATE_EARLY", v, sizeof v);
    if (!v[0] && g_proxy) v[0] = '1';   /* the pdh.dll proxy is early by nature: same as launch.py */
    MEMORY_BASIC_INFORMATION mbi;
    int image_there = VirtualQuery((void *)0x800000000ull, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT;
    if (!v[0] || image_there) return;
    g_early = 1;
    g_base = (uint8_t *)GetModuleHandleA(NULL);
    gate_init();
    if (v[0] == '1') {
        char sp[16] = "";
        if (GetEnvironmentVariableA("BBSTATE_SPIN", sp, sizeof sp)) vs_set_spin(atoi(sp));
        if (!vs_init(blog)) { blog("early: vsync table not allocated: no virtualization"); }
        else g_virt = 1;
    }
    const char *err = install_resolve_hook();
    if (err) { g_virt = 0; blog("early: resolver not hooked: %s", err); }
    else blog("early: resolver hooked (stub %p), handle virtualization %s", g_tramp - 64, g_virt ? "ON" : "off");
    g_main_tid = GetCurrentThreadId();
    const char *ferr = fs_reserve();
    const char *cerr = install_canon();
    if (cerr) { blog("early: canonical stack depth not installed: %s", cerr); g_fs_on = 0; }
    else blog("early: canonical stack depth: main thread 0x%llx, guest threads 0x%llx below the top", canon_main_depth, canon_thread_depth);
    if (!cerr && g_fs_on) g_canon = 2;
    blog("early: fixed stack addresses %s", g_fs_on ? "ON (arena 0x200000000)" : ferr ? ferr : "off");
}
/* Early mode: the worker waits for the game (image relocated, first frames) before the usual setup. */
static int wait_game_started(void) {
    for (int i = 0; i < 6000; ++i) {   /* 10 min */
        __try {
            uint64_t *slot = (uint64_t *)SLOT_SUBMITDONE;
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(slot, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && *slot == (uint64_t)g_base + RVA_FN_SUBMITDONE &&
                *(uint64_t *)(g_base + RVA_FRAMES_SUBMITTED) > 0) return 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Sleep(100);
    }
    return 0;
}

/* ---- frame hook (called from thunks.asm on the guest thread) ---- */
static volatile LONG g_ov_hold, g_ov_held;   /* overlay open with "pause game": the render thread waits here between frames */
static HANDLE g_ev_wake;
void bbs_pre(int kind, uint64_t frame) {
    InterlockedIncrement64(&g_count[kind]);
    g_last_tid[kind] = (LONG)GetCurrentThreadId();
    if (kind == g_pause_kind && !g_state) prac_frame();   /* practice work that must run guest code on a game thread */
    if (kind == g_pause_kind && g_ov_hold && g_state != 1) {
        InterlockedExchange(&g_ov_held, 1);
        while (g_ov_hold && g_state != 1) { WaitForSingleObject(g_ev_wake, 50); if (!g_state) prac_frame(); }
        InterlockedExchange(&g_ov_held, 0);
    }
    if (kind != g_pause_kind || g_state != 1) return;
    if (InterlockedCompareExchange(&g_state, 2, 1) != 1) return;
    g_pause_frame = frame;
    g_pause_stackbase = __readgsqword(0x08);
    g_pause_tid = GetCurrentThreadId();
    SetEvent(g_ev_paused);
    WaitForSingleObject(g_ev_resume, INFINITE);
}
static int pause_main(DWORD timeout) {
    ResetEvent(g_ev_paused); ResetEvent(g_ev_resume);
    InterlockedExchange(&g_state, 1);
    if (g_ev_wake) SetEvent(g_ev_wake);
    if (WaitForSingleObject(g_ev_paused, timeout) == WAIT_OBJECT_0) return 1;
    if (InterlockedCompareExchange(&g_state, 0, 1) == 1) return 0;
    WaitForSingleObject(g_ev_paused, INFINITE); /* it got there just now */
    return 1;
}
static void release_main(void) { InterlockedExchange(&g_state, 0); SetEvent(g_ev_resume); }

/* ---- host objects kept alive ----
 * Guest mutexes/conds/attrs are pointers in guest memory to host heap objects. Restoring memory
 * brings back handles to objects the game destroyed after the snapshot (first load died on "Invalid
 * mutex"). Creation is hooked: each handle is recorded with the snapshot generation g_gen, the
 * guest address it was written to, its type and a sequence number. A destroy of a handle made after
 * the latest snapshot goes to the original; an older one is kept (host object never freed, guest
 * handle reset as the original would). Rwlocks, semaphores and thread attrs are rare: always kept. */
#define OERR(n) ((int32_t)(0x80020000u | (n)))
#define FORWARD (1ll << 32)
static volatile LONG g_kept[10], g_freed[6];
static volatile LONG g_gen;
static volatile LONG g_obj_seq;
static int32_t posix_code(int32_t r) { return r ? (int32_t)((uint32_t)r & 0xffff) : 0; }
uint64_t keep_iorig[6], keep_dorig[6];

typedef struct { uint64_t key, where; uint32_t gen, seq, type, pad; } Ent; /* 32 B */
#define TAB_BITS 17
#define TAB_SIZE (1u << TAB_BITS)
static Ent *g_tab;
static SRWLOCK g_tab_lock = SRWLOCK_INIT;
static LONG g_tab_count;
typedef struct { uint64_t key, where; uint32_t seq, type; } Kept; /* objects kept alive (for L3 notes) */
#define KEPT_CAP 65536
static Kept *g_keptlist; static volatile LONG g_nkept;
static uint32_t tab_hash(uint64_t k) { k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33; return (uint32_t)k & (TAB_SIZE - 1); }
static void tab_put(uint64_t k, uint64_t where, uint32_t gen, uint32_t type) {
    AcquireSRWLockExclusive(&g_tab_lock);
    if (g_tab_count < (LONG)(TAB_SIZE / 2)) {
        uint32_t i = tab_hash(k);
        while (g_tab[i].key && g_tab[i].key != k) i = (i + 1) & (TAB_SIZE - 1);
        if (!g_tab[i].key) ++g_tab_count;
        g_tab[i].key = k; g_tab[i].where = where; g_tab[i].gen = gen; g_tab[i].type = type;
        g_tab[i].seq = (uint32_t)InterlockedIncrement(&g_obj_seq);
    }
    ReleaseSRWLockExclusive(&g_tab_lock);
}
/* Removes k; returns its entry (gen -1 if absent). Backward-shift deletion keeps probe chains intact. */
static Ent tab_take(uint64_t k) {
    Ent r = {0, 0, 0xffffffffu, 0, 0, 0};
    AcquireSRWLockExclusive(&g_tab_lock);
    uint32_t i = tab_hash(k);
    while (g_tab[i].key && g_tab[i].key != k) i = (i + 1) & (TAB_SIZE - 1);
    if (g_tab[i].key) {
        r = g_tab[i]; --g_tab_count;
        for (uint32_t j = i;;) {
            g_tab[i].key = 0;
            for (;;) {
                j = (j + 1) & (TAB_SIZE - 1);
                if (!g_tab[j].key) goto done;
                uint32_t h = tab_hash(g_tab[j].key);
                if (i <= j ? (i < h && h <= j) : (i < h || h <= j)) continue;
                break;
            }
            g_tab[i] = g_tab[j]; i = j;
        }
    }
done:
    ReleaseSRWLockExclusive(&g_tab_lock);
    return r;
}
static void kept_add(uint64_t key, uint64_t where, uint32_t seq, uint32_t type) {
    LONG i = InterlockedIncrement(&g_nkept) - 1;
    if (i < KEPT_CAP) { g_keptlist[i].key = key; g_keptlist[i].where = where; g_keptlist[i].seq = seq; g_keptlist[i].type = type; }
}
void keep_on_init(int32_t result, uint64_t *out, int idx) {
    if (!result && out && *out >= 3) tab_put(*out, (uint64_t)out, (uint32_t)g_gen, (uint32_t)idx);
}
/* idx: 0 mutex, 1 posix mutex, 2 cond, 3 posix cond, 4 mutex attr, 5 posix mutex attr */
int64_t keep_on_destroy(uint64_t *h, int idx) {
    int is_mutex = idx < 2, is_cond = idx == 2 || idx == 3;
    if (!h || (is_mutex ? *h < 3 : is_cond ? *h < 2 : !*h)) return FORWARD; /* errors, static init */
    Ent e = tab_take(*h);
    if (e.gen == (uint32_t)g_gen) { InterlockedIncrement(&g_freed[idx]); return FORWARD; }
    InterlockedIncrement(&g_kept[idx / 2]);
    kept_add(*h, (uint64_t)h, e.seq, (uint32_t)idx);
    *h = is_mutex ? 2 : 0;
    return 0;
}
int32_t keep_rw_destroy_c(uint64_t *h) {
    InterlockedIncrement(&g_kept[3]);
    if (!h) return OERR(22);
    if (*h) { kept_add(*h, (uint64_t)h, 0, 6); *h = 1; }
    return 0;
}
int32_t keep_sem_delete_c(uint64_t id) { InterlockedIncrement(&g_kept[4]); kept_add(id, 0, 0, 7); return 0; }
int32_t keep_tattr_destroy_c(uint64_t *a) {
    InterlockedIncrement(&g_kept[5]);
    if (!a || !*a) return OERR(22);
    kept_add(*a, (uint64_t)a, 0, 8);
    *a = 0;
    return 0;
}
int32_t keep_posix_tattr_destroy_c(uint64_t *a) { return posix_code(keep_tattr_destroy_c(a)); }
extern void track_init0(void), track_init1(void), track_init2(void), track_init3(void), track_init4(void), track_init5(void);
extern void track_destroy0(void), track_destroy1(void), track_destroy2(void), track_destroy3(void), track_destroy4(void), track_destroy5(void);
extern void keep_rw_destroy(void), keep_sem_delete(void), keep_tattr_destroy(void), keep_posix_tattr_destroy(void);
static const struct { uint64_t rva; void (*fn)(void); const char *name; uint64_t *orig; } k_keep[] = {
    {RVA_KEEP_MUTEX_DESTROY, track_destroy0, "mutex_destroy", &keep_dorig[0]},
    {RVA_KEEP_POSIX_MUTEX_DESTROY, track_destroy1, "posix_mutex_destroy", &keep_dorig[1]},
    {RVA_KEEP_COND_DESTROY, track_destroy2, "cond_destroy", &keep_dorig[2]},
    {RVA_KEEP_POSIX_COND_DESTROY, track_destroy3, "posix_cond_destroy", &keep_dorig[3]},
    {RVA_KEEP_MATTR_DESTROY, track_destroy4, "mutexattr_destroy", &keep_dorig[4]},
    {RVA_KEEP_POSIX_MATTR_DESTROY, track_destroy5, "posix_mutexattr_destroy", &keep_dorig[5]},
    {RVA_INIT_MUTEX, track_init0, "mutex_init", &keep_iorig[0]},
    {RVA_INIT_POSIX_MUTEX, track_init1, "posix_mutex_init", &keep_iorig[1]},
    {RVA_INIT_COND, track_init2, "cond_init", &keep_iorig[2]},
    {RVA_INIT_POSIX_COND, track_init3, "posix_cond_init", &keep_iorig[3]},
    {RVA_INIT_MATTR, track_init4, "mutexattr_init", &keep_iorig[4]},
    {RVA_INIT_POSIX_MATTR, track_init5, "posix_mutexattr_init", &keep_iorig[5]},
    {RVA_KEEP_RW_DESTROY, keep_rw_destroy, "rwlock_destroy", NULL},
    {RVA_KEEP_SEM_DELETE, keep_sem_delete, "sema_delete", NULL},
    {RVA_KEEP_TATTR_DESTROY, keep_tattr_destroy, "thread_attr_destroy", NULL},
    {RVA_KEEP_POSIX_TATTR_DESTROY, keep_posix_tattr_destroy, "posix_thread_attr_destroy", NULL},
};
/* ---- gate: the only addresses of this DLL that guest memory ever holds ----
 * Import slots get an entry of this page (jmp [rip] to the DLL function) instead of the function: the
 * entries come in a fixed order (frame hooks, vsync in vs_entries order, keep thunks), so a slot saved
 * by one build of the DLL names the same functions in another build (format version, not build). */
#define GATE_ENTRY 16
#define GATE_MAX 128
static int g_ngate;
static const void *g_gate_fn[GATE_MAX];
static void gate_add(const void *fn) {
    if (g_ngate == GATE_MAX) return;
    uint8_t *e = g_gate + g_ngate * GATE_ENTRY;
    e[0] = 0xff; e[1] = 0x25; memset(e + 2, 0, 4); *(uint64_t *)(e + 6) = (uint64_t)fn; e[14] = e[15] = 0xcc;
    g_gate_fn[g_ngate++] = fn;
}
static void gate_init(void) {
    if (g_gate) return;
    g_gate = (uint8_t *)VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_gate) return;
    for (int k = 0; k < 3; ++k) gate_add((const void *)k_hooks[k]);
    void (*vs[64])(void); int nvs = vs_entries(vs, 64);
    for (int k = 0; k < nvs; ++k) gate_add((const void *)vs[k]);
    for (int k = 0; k < (int)(sizeof k_keep / sizeof *k_keep); ++k) gate_add((const void *)k_keep[k].fn);
    gate_add((const void *)vc_tsc); gate_add((const void *)vc_kclock); gate_add((const void *)vc_pclock);   /* new entries only here */
    DWORD old; VirtualProtect(g_gate, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), g_gate, 4096);
}
static uint64_t gate_of(const void *fn) {
    for (int i = 0; i < g_ngate; ++i) if (g_gate_fn[i] == fn) return (uint64_t)(g_gate + i * GATE_ENTRY);
    return (uint64_t)fn;
}
static struct { uint64_t *slot, orig; } g_keep_slots[256];
static int g_nkeep_slots;
/* Finds the import slots (eboot data) that point at the hooked functions and redirects them. */
static void install_keep(int on) {
    if (!on) {
        for (int i = 0; i < g_nkeep_slots; ++i)
            InterlockedCompareExchange64((volatile LONG64 *)g_keep_slots[i].slot, (LONG64)g_keep_slots[i].orig, *(LONG64 *)g_keep_slots[i].slot);
        g_nkeep_slots = 0;
        return;
    }
    int per[16] = {0};
    if (!g_tab) g_tab = (Ent *)VirtualAlloc(NULL, sizeof(Ent) * TAB_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_keptlist) g_keptlist = (Kept *)VirtualAlloc(NULL, sizeof(Kept) * KEPT_CAP, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_tab || !g_keptlist) { blog("keep: no table, not installed"); return; }
    for (int k = 0; k < (int)(sizeof k_keep / sizeof *k_keep); ++k) if (k_keep[k].orig) *k_keep[k].orig = (uint64_t)g_base + k_keep[k].rva;
    for (uint64_t a = 0x800000000ull, end = low_next(); a < end;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)a, &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) && !(mbi.Protect & PAGE_GUARD)) {
            uint64_t *p = (uint64_t *)mbi.BaseAddress, *e = (uint64_t *)((uint8_t *)mbi.BaseAddress + mbi.RegionSize);
            for (; p < e; ++p)
                for (int k = 0; k < (int)(sizeof k_keep / sizeof *k_keep); ++k)
                    if (*p == (uint64_t)g_base + k_keep[k].rva && g_nkeep_slots < 256) {
                        g_keep_slots[g_nkeep_slots].slot = p; g_keep_slots[g_nkeep_slots].orig = *p; ++g_nkeep_slots;
                        InterlockedExchange64((volatile LONG64 *)p, (LONG64)gate_of((const void *)k_keep[k].fn));
                        ++per[k];
                    }
        }
        a = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
    }
    char b[512]; int n = 0;
    for (int k = 0; k < (int)(sizeof k_keep / sizeof *k_keep); ++k) n += _snprintf_s(b + n, sizeof b - n, _TRUNCATE, " %s=%d", k_keep[k].name, per[k]);
    blog("keep slots:%s", b);
}

static int install_hooks(int on) {
    for (int k = 0; k < 3; ++k) {
        uint64_t *slot = (uint64_t *)k_slots[k];
        DWORD old;
        if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) { blog("VirtualProtect slot %d failed %lu", k, GetLastError()); return 0; }
        if (on) {
            uint64_t want = (uint64_t)g_base + k_fn_rva[k];
            if (*slot != want) { blog("slot %s = 0x%llx, expected 0x%llx: not hooking", k_names[k], *slot, want); VirtualProtect(slot, 8, old, &old); return 0; }
            bbs_orig[k] = *slot;
            InterlockedExchange64((volatile LONG64 *)slot, (LONG64)gate_of((const void *)k_hooks[k]));
        } else if (*slot == gate_of((const void *)k_hooks[k])) {
            InterlockedExchange64((volatile LONG64 *)slot, (LONG64)bbs_orig[k]);
        }
        VirtualProtect(slot, 8, old, &old);
    }
    g_hooked = on;
    return 1;
}

/* ---- GPU ---- */
static int gpu_idle(uint32_t *ns, int *wr, int *sl) {
    uint8_t *lp = *(uint8_t *volatile *)(g_base + RVA_LIVERPOOL);
    if (!lp) return 0;
    *ns = *(volatile uint32_t *)(lp + LIVERPOOL_NUM_SUBMITS);
    *wr = *(volatile uint8_t *)(lp + LIVERPOOL_WORK_RETIRED);
    *sl = *(volatile uint8_t *)(g_base + RVA_SUBMISSION_LOCK);
    return *ns == 0 && *wr && !*sl;
}
static int wait_gpu_idle(double timeout_ms, double *waited) {
    double t0 = now_ms();
    uint32_t ns = 0; int wr = 0, sl = 0;
    while (!gpu_idle(&ns, &wr, &sl)) {
        if (now_ms() - t0 > timeout_ms) { blog("GPU not idle after %.0f ms: num_submits=%u work_retired=%d submission_lock=%d", timeout_ms, ns, wr, sl); return 0; }
        Sleep(0);
    }
    *waited = now_ms() - t0;
    return 1;
}
/* Task 6.1: host work that goes on after the guest is stopped and Liverpool counts as idle (disassembly):
 * the Vulkan scheduler's GPU timeline (master semaphore ticks) and its pending operations (host callbacks
 * run when a tick completes), host copies and the deferred signals waiting for them (EOP fences after host
 * copies), Liverpool's pending fences (label writes waiting for a tick), the BbCopy copy pool. */
typedef struct { uint64_t tick_done, tick_cur, ops, ops_n, hc_issued, hc_done, df_issued, df_done, pfences, bc_tasks, bc_epochs, bc_b0, bc_b8, pwrites; int ok; } HostQ;
static HostQ host_q(void) {
    HostQ q; memset(&q, 0, sizeof q);
    __try {
        uint8_t *lp = *(uint8_t *volatile *)(g_base + RVA_LIVERPOOL);
        uint8_t *mm = *(uint8_t **)(g_base + RVA_MEMMGR_INSTANCE);
        uint8_t *rast = mm ? *(uint8_t **)(mm + 8) : NULL;
        uint8_t *s = rast ? *(uint8_t **)(rast + RAST_SCHEDULER) : NULL;
        if (lp) q.pfences = *(volatile uint64_t *)(lp + LIVERPOOL_PFENCE_SIZE);
        if (rast) q.pwrites = (*(volatile uint64_t *)(rast + RAST_PENDING_WRITES + 8) - *(volatile uint64_t *)(rast + RAST_PENDING_WRITES)) / 24;   /* vector<PendingWrite {lo, hi, tick}> */
        if (s) {
            q.tick_done = *(volatile uint64_t *)(s + SCHED_TICK_DONE); q.tick_cur = *(volatile uint64_t *)(s + SCHED_TICK_CUR);
            q.ops = *(volatile uint64_t *)(s + SCHED_PENDING_OPS); q.ops_n = *(volatile uint32_t *)(s + SCHED_PENDING_OPS_N);
            q.hc_issued = *(volatile uint64_t *)(s + SCHED_HCOPY_ISSUED); q.hc_done = *(volatile uint64_t *)(s + SCHED_HCOPY_DONE);
            q.df_issued = *(volatile uint64_t *)(s + SCHED_DEFER_ISSUED);
            uint64_t *dp = *(uint64_t **)(s + SCHED_DEFER_DONE_PTR); q.df_done = dp ? *(volatile uint64_t *)dp : 0;
            q.ok = 1;
        }
        if (*(volatile uint8_t *)(g_base + RVA_BBCOPY_POOL_GUARD)) {
            uint8_t *p = g_base + RVA_BBCOPY_POOL;
            q.bc_tasks = *(volatile uint64_t *)(p + BBCOPY_TASKS); q.bc_epochs = *(volatile uint64_t *)(p + BBCOPY_EPOCHS);
            q.bc_b0 = *(volatile uint64_t *)(p + BBCOPY_B0); q.bc_b8 = *(volatile uint8_t *)(p + BBCOPY_B8);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { q.ok = 0; }
    return q;
}
/* Work in flight: GPU ticks not completed, pending host callbacks, host copies or deferred signals not done,
 * pending fences, copy pool tasks (one epoch is the pool's normal state). */
static int host_q_busy(const HostQ *q) {
    return q->ok && (q->tick_done + 1 < q->tick_cur || q->ops || q->hc_done < q->hc_issued || q->df_done < q->df_issued || q->pfences || q->bc_tasks || q->bc_epochs > 1);
}
static HostQ g_hostq_at_point;
static void host_q_str(const HostQ *q, char *out, size_t n) {
    _snprintf_s(out, n, _TRUNCATE, "GPU ticks done %llu / current %llu, pending ops %llu (%llu), host copies %llu/%llu, deferred signals %llu/%llu, pending fences %llu, copy pool tasks %llu epochs %llu b0 %llu b8 %llu, pending GPU writes %llu%s",
                q->tick_done, q->tick_cur, q->ops, q->ops_n, q->hc_done, q->hc_issued, q->df_done, q->df_issued, q->pfences, q->bc_tasks, q->bc_epochs, q->bc_b0, q->bc_b8, q->pwrites,
                !q->ok ? " (scheduler not found)" : host_q_busy(q) ? " BUSY" : "");
}

/* ---- VideoOut and event queues ----
 * The game's render loop does GetFlipStatus() and waits for a Flip event while status.flipArg (host)
 * lags its own last flip argument (guest memory). Restoring guest memory without this host state hung
 * the game whenever the slot was "ahead" of the current timeline (task #3). So the consistent point
 * also needs no flip in flight, and a slot keeps flip_status, the buffer labels and every equeue
 * event's triggered flag/data, which a load puts back. */
typedef struct { uint64_t count, process_time, tsc; int64_t flip_arg; uint64_t submit_tsc, reserved0; int32_t gc_queue_num, flip_pending_num, current_buffer; uint32_t reserved1; } FlipStatus;
typedef struct { int32_t eq; int16_t filter; uint16_t flags; uint64_t ident, data; uint32_t trig, pad; } VOEv;
#define VO_MAX_EV 64
typedef struct { uint32_t magic, version, nev, ok; FlipStatus flip; uint64_t vblank[3]; uint64_t labels[16]; int32_t prev_index, pad; VOEv ev[VO_MAX_EV]; } VOState;
typedef void (*CxxFn)(void *);
static uint8_t *vo_port(void) {
    uint8_t *drv = *(uint8_t **)(g_base + RVA_VO_DRIVER);
    return drv ? ((uint8_t *(*)(void *, int))(g_base + RVA_VO_GETPORT))(drv, 1) : NULL;
}
static void cxx_lock(void *m) { ((CxxFn)(g_base + RVA_MUTEX_LOCK))(m); }
static void cxx_unlock(void *m) { ((CxxFn)(g_base + RVA_MUTEX_UNLOCK))(m); }
static int vo_flips_idle(void) {
    uint8_t *port = vo_port();
    if (!port) return 1;
    volatile FlipStatus *f = (volatile FlipStatus *)(port + VO_FLIP_STATUS);
    return f->flip_pending_num == 0 && f->gc_queue_num == 0;
}
/* kqueues: libc++ unordered_map<s32, EqueueInternal*>, first node at +16: {next, hash, key, value} */
static int vo_equeues(int32_t *keys, uint8_t **eqs, int cap) {
    int n = 0;
    for (uint8_t *node = *(uint8_t **)(g_base + RVA_KQUEUES + 16); node && n < cap; node = *(uint8_t **)node) {
        keys[n] = *(int32_t *)(node + 16); eqs[n] = *(uint8_t **)(node + 24); ++n;
    }
    return n;
}
static void vo_capture(VOState *s) {
    memset(s, 0, sizeof *s);
    s->magic = 0x4F564242u; s->version = 1;
    uint8_t *port = vo_port();
    if (!port) return;
    cxx_lock(port + VO_PORT_MUTEX);
    memcpy(&s->flip, port + VO_FLIP_STATUS, sizeof s->flip);
    memcpy(s->vblank, port + VO_VBLANK_STATUS, sizeof s->vblank);
    memcpy(s->labels, port + VO_LABELS, sizeof s->labels);
    s->prev_index = *(int32_t *)(port + VO_PREV_INDEX);
    cxx_unlock(port + VO_PORT_MUTEX);
    int32_t keys[16]; uint8_t *eqs[16];
    int ne = vo_equeues(keys, eqs, 16);
    for (int i = 0; i < ne; ++i) {
        cxx_lock(eqs[i] + EQ_MUTEX);
        uint8_t *b = *(uint8_t **)(eqs[i] + EQ_EVENTS), *e = *(uint8_t **)(eqs[i] + EQ_EVENTS + 8);
        for (uint8_t *p = b; p < e && s->nev < VO_MAX_EV; p += EQ_EVENT_SIZE) {
            VOEv *v = &s->ev[s->nev++];
            v->eq = keys[i]; v->ident = *(uint64_t *)p; v->filter = *(int16_t *)(p + 8); v->flags = *(uint16_t *)(p + 10);
            v->data = *(uint64_t *)(p + 16); v->trig = *(uint8_t *)(p + EQ_EV_TRIGGERED);
        }
        cxx_unlock(eqs[i] + EQ_MUTEX);
    }
    s->ok = 1;
}
/* Called with guest threads suspended; waiters are woken by vo_wake after they resume. */
static void vo_restore(const VOState *s, char *note, size_t nn) {
    uint8_t *port = vo_port();
    if (!s || s->magic != 0x4F564242u || !s->ok || !port) { _snprintf_s(note, nn, _TRUNCATE, "no VideoOut state in the slot"); return; }
    cxx_lock(port + VO_PORT_MUTEX);
    FlipStatus *f = (FlipStatus *)(port + VO_FLIP_STATUS), was = *f;
    FlipStatus nw = s->flip;
    nw.gc_queue_num = f->gc_queue_num; nw.flip_pending_num = f->flip_pending_num; /* both 0 at a consistent point */
    *f = nw;
    memcpy(port + VO_LABELS, s->labels, sizeof s->labels);
    *(int32_t *)(port + VO_PREV_INDEX) = s->prev_index;
    cxx_unlock(port + VO_PORT_MUTEX);
    int32_t keys[16]; uint8_t *eqs[16];
    int ne = vo_equeues(keys, eqs, 16), set = 0, missing = 0;
    for (uint32_t k = 0; k < s->nev; ++k) {
        const VOEv *v = &s->ev[k]; int done = 0;
        for (int i = 0; i < ne && !done; ++i) {
            if (keys[i] != v->eq) continue;
            cxx_lock(eqs[i] + EQ_MUTEX);
            uint8_t *b = *(uint8_t **)(eqs[i] + EQ_EVENTS), *e = *(uint8_t **)(eqs[i] + EQ_EVENTS + 8);
            for (uint8_t *p = b; p < e; p += EQ_EVENT_SIZE)
                if (*(uint64_t *)p == v->ident && *(int16_t *)(p + 8) == v->filter) { *(uint64_t *)(p + 16) = v->data; *(uint8_t *)(p + EQ_EV_TRIGGERED) = (uint8_t)v->trig; done = 1; break; }
            cxx_unlock(eqs[i] + EQ_MUTEX);
        }
        if (done) ++set; else ++missing;
    }
    _snprintf_s(note, nn, _TRUNCATE, "flip count %llu->%llu arg %lld->%lld buffer %d->%d; %d equeue events restored%s",
                was.count, nw.count, was.flip_arg, nw.flip_arg, was.current_buffer, nw.current_buffer, set, missing ? " (some missing now!)" : "");
}
static void vo_wake(void) {
    int32_t keys[16]; uint8_t *eqs[16];
    int ne = vo_equeues(keys, eqs, 16);
    for (int i = 0; i < ne; ++i) ((CxxFn)(g_base + RVA_CV_NOTIFY_ALL))(eqs[i] + EQ_COND);
}

/* ---- guest files ----
 * Guest fds index the runtime's File table; the read position lives in the host CRT descriptor. A
 * slot keeps every open fd with its path and position. A load puts the positions back, reopens game
 * files (/app0) that were closed since (under the same fd number) and closes fds that hold another
 * file now. Anything else that differs (writable files) refuses the load. */
typedef struct { int32_t fd, pad; int64_t pos; char path[240]; } FRec;
#define FREC_MAX 256
typedef struct { uint32_t magic, n; FRec f[FREC_MAX]; } FState;
static uint8_t *file_entry(int fd) { return g_base + RVA_FILES + (uint64_t)fd * FILE_SIZE; }
static int64_t rt_lseek(int fd, int64_t o, int w) { return ((int64_t (*)(int, int64_t, int))(g_base + RVA_FILE_LSEEK))(fd, o, w); }
static int64_t rt_open(const char *p) { return ((int64_t (*)(const char *, int, int))(g_base + RVA_FILE_OPEN))(p, 0, 0); }
static int64_t rt_close(int fd) { return ((int64_t (*)(int))(g_base + RVA_FILE_CLOSE))(fd); }
static int file_used(int fd) { return *(int32_t *)file_entry(fd); }
static const char *file_path(int fd) { return (const char *)file_entry(fd) + FILE_PATH; }
static void files_capture(FState *s) {
    s->magic = 0x4C464242u; s->n = 0;
    for (int fd = 3; fd < FILE_MAX && s->n < FREC_MAX; ++fd) {
        if (!file_used(fd)) continue;
        FRec *r = &s->f[s->n++];
        r->fd = fd; strncpy_s(r->path, sizeof r->path, file_path(fd), _TRUNCATE);
        r->pos = *(uint64_t *)(file_entry(fd) + FILE_DIR) ? -1 : rt_lseek(fd, 0, 1);
    }
}
static int game_file(const char *p) { return !strncmp(p, "/app0/", 6); }
/* apply = 0: only checks (game running); 1: makes the table match (game paused). */
static int files_restore(const FState *s, int apply, char *note, size_t nn) {
    int repos = 0, reopened = 0, closed = 0, extra = 0;
    if (!s || s->magic != 0x4C464242u) { _snprintf_s(note, nn, _TRUNCATE, "no file table in the slot"); return 1; }
    for (uint32_t i = 0; i < s->n; ++i) {
        const FRec *r = &s->f[i];
        int same = file_used(r->fd) && !strcmp(file_path(r->fd), r->path);
        if (!same && !game_file(r->path)) { _snprintf_s(note, nn, _TRUNCATE, "file %s (fd %d) was open at save and is not now", r->path, r->fd); return 0; }
        if (!same && file_used(r->fd) && !game_file(file_path(r->fd))) { _snprintf_s(note, nn, _TRUNCATE, "fd %d now holds %s", r->fd, file_path(r->fd)); return 0; }
    }
    if (!apply) { _snprintf_s(note, nn, _TRUNCATE, "files ok"); return 1; }
    for (uint32_t i = 0; i < s->n; ++i) {
        const FRec *r = &s->f[i];
        if (!(file_used(r->fd) && !strcmp(file_path(r->fd), r->path))) {
            if (file_used(r->fd)) { rt_close(r->fd); ++closed; }
            int64_t fd = rt_open(r->path);
            if (fd < 0) { _snprintf_s(note, nn, _TRUNCATE, "cannot reopen %s: %lld", r->path, fd); return 0; }
            if (fd != r->fd) { /* move the new entry to the slot's fd number */
                memcpy(file_entry(r->fd), file_entry((int)fd), FILE_SIZE);
                memset(file_entry((int)fd), 0, FILE_SIZE);
            }
            ++reopened;
        }
        if (r->pos >= 0 && rt_lseek(r->fd, 0, 1) != r->pos) { rt_lseek(r->fd, r->pos, 0); ++repos; }
    }
    for (int fd = 3; fd < FILE_MAX; ++fd) {
        if (!file_used(fd)) continue;
        uint32_t i; for (i = 0; i < s->n && s->f[i].fd != fd; ++i) {}
        if (i == s->n) { if (game_file(file_path(fd))) { rt_close(fd); ++closed; } else ++extra; }
    }
    _snprintf_s(note, nn, _TRUNCATE, "%u files: %d positions restored, %d reopened, %d closed, %d extra left open", s->n, repos, reopened, closed, extra);
    return 1;
}

/* ---- kernel semaphores ----
 * Counts live in the runtime's registry (host), not in guest memory. A job signalled but not yet
 * taken at save time, or at load time, desynchronises workers from the restored job queues: a load
 * of a slot saved right after a respawn crashed in CSChrThread until counts were restored. Too low
 * now: the runtime's own sem_signal adds the difference (and wakes waiters); too high: lowered. */
typedef struct { uint32_t id; int32_t count; } SRec;
#define SREC_MAX 512
typedef struct { uint32_t magic, n; SRec s[SREC_MAX]; } SemState;
extern int64_t sysv_call2(void *fn, uint64_t a, uint64_t b);
static void sema_capture(SemState *st) {
    st->magic = 0x4D534242u; st->n = 0;
    for (uint8_t *p = *(uint8_t **)(g_base + RVA_SEMA_REGISTRY); p && st->n < SREC_MAX; p = *(uint8_t **)(p + SEMA_NEXT)) {
        st->s[st->n].id = *(uint32_t *)p; st->s[st->n].count = *(int32_t *)(p + SEMA_COUNT); ++st->n;
    }
}
static void sema_restore(const SemState *st, char *note, size_t nn) {
    int up = 0, down = 0, gone = 0, nonzero = 0;
    if (!st || st->magic != 0x4D534242u) { _snprintf_s(note, nn, _TRUNCATE, "no semaphore state in the slot"); return; }
    char det[200]; int dn = 0; det[0] = 0;
    for (uint32_t i = 0; i < st->n; ++i) {
        uint8_t *p;
        for (p = *(uint8_t **)(g_base + RVA_SEMA_REGISTRY); p && *(uint32_t *)p != st->s[i].id; p = *(uint8_t **)(p + SEMA_NEXT)) {}
        if (!p) { ++gone; continue; }
        int32_t cur = *(volatile int32_t *)(p + SEMA_COUNT), want = st->s[i].count;
        if (want) ++nonzero;
        if (cur == want) continue;
        if (dn < 180) dn += _snprintf_s(det + dn, sizeof det - dn, _TRUNCATE, " %u:%d->%d", st->s[i].id, cur, want);
        if (cur < want) { sysv_call2(g_base + RVA_SEM_SIGNAL, st->s[i].id, (uint64_t)(want - cur)); ++up; }
        else { *(volatile int32_t *)(p + SEMA_COUNT) = want; ++down; }
    }
    _snprintf_s(note, nn, _TRUNCATE, "%u semaphores (%d non-zero in the slot): %d signalled, %d lowered, %d missing;%s", st->n, nonzero, up, down, gone, det);
}

/* ---- audio decoders (runtime_ajm.c) ----
 * The port decodes the game's ATRAC9 sound streams itself: per AJM instance it keeps the stream's gapless
 * counters (samples left to output, samples to skip), the position in the current superframe and the
 * decoder's own state (overlap of the last frame), in host memory. A load put the game's streams back but
 * left these counters "ahead": sounds stopped halfway, a looping wind sound repeated one piece (the user
 * heard it after F9, task 6). A slot keeps every context's instance table, the decoder state of every
 * instance and the batch table; a load writes them back. Decoder state is one calloc'd block per instance
 * (0x168c0 bytes, pointers into itself): a load reuses the instance's block of now or one allocated before
 * the pause, and frees the blocks nobody uses after the game runs again (no heap calls while paused). */
#define AJM_CTX_MAX 9
#define AJM_CTX_HDR 0x68
#define AJM_INST 0x68
#define AJM_NINST 0x4000
#define AJM_I_HANDLE 0x18
#define AT9_SIZE 0x168c0ull
#define AJM_BATCH_BYTES (256 * 12)
#define EXT_AJM 0x534D4A41u /* 'AJMS' */
typedef struct { uint32_t magic, nrec; uint8_t batches[AJM_BATCH_BYTES]; } AjmHdr;
typedef struct { int32_t ctx, inst; uint64_t handle; uint8_t data[AJM_INST]; } AjmRec;   /* inst -1: the context's header; AT9_SIZE bytes follow when handle != 0 */
static uint8_t *g_ajm; static uint64_t g_ajm_len, g_ajm_cap = 48ull << 20;
static int g_ajm_on = 1;     /* debug: command "ajmstate 0" loads leave the audio decoders as they are */
static int g_ajm_trunc;
static uint8_t **ajm_contexts(void) { return (uint8_t **)(g_base + RVA_AJM_CONTEXTS); }
static void ajm_capture(void) {
    AjmHdr *h = (AjmHdr *)g_ajm; uint64_t off = sizeof *h;
    h->magic = 0x4D4A4142u; h->nrec = 0; g_ajm_trunc = 0;
    memcpy(h->batches, g_base + RVA_AJM_BATCHES, AJM_BATCH_BYTES);
    for (int c = 1; c < AJM_CTX_MAX; ++c) {
        uint8_t *ctx = ajm_contexts()[c];
        if (!ctx) continue;
        for (int i = -1; i < AJM_NINST; ++i) {
            uint8_t *src = i < 0 ? ctx : ctx + AJM_CTX_HDR + (uint64_t)i * AJM_INST;
            if (i >= 0 && !*(int32_t *)src) continue;
            uint64_t hd = i < 0 ? 0 : *(uint64_t *)(src + AJM_I_HANDLE), need = sizeof(AjmRec) + (hd ? AT9_SIZE : 0);
            if (off + need > g_ajm_cap) { g_ajm_trunc = 1; break; }
            AjmRec *r = (AjmRec *)(g_ajm + off); r->ctx = c; r->inst = i; r->handle = hd; memcpy(r->data, src, AJM_INST);
            off += sizeof *r;
            if (hd) { memcpy(g_ajm + off, (void *)hd, AT9_SIZE); off += AT9_SIZE; }
            ++h->nrec;
        }
    }
    g_ajm_len = off;
}
static uint64_t g_at9_spare[512]; static int g_nat9_spare;     /* blocks allocated before a load's pause */
static uint64_t g_at9_free[1024]; static int g_nat9_free;      /* blocks to free after it */
static void *(*at9_get(void))(void) { return (void *(*)(void))(g_base + RVA_ATRAC9_GET_HANDLE); }
static const uint8_t *layout_ext(const uint8_t *b, uint32_t len, uint32_t tag, uint64_t *out_len);
/* Before the pause: enough spare decoder blocks for every instance of the slot that has one. */
static void ajm_prepare(const uint8_t *b, uint32_t len) {
    uint64_t al = 0; const AjmHdr *h = (const AjmHdr *)layout_ext(b, len, EXT_AJM, &al);
    if (!h || al < sizeof *h || h->magic != 0x4D4A4142u) return;
    int need = 0;
    for (uint64_t off = sizeof *h; off + sizeof(AjmRec) <= al;) { const AjmRec *r = (const AjmRec *)((const uint8_t *)h + off); need += r->handle != 0; off += sizeof *r + (r->handle ? AT9_SIZE : 0); }
    while (g_nat9_spare < need && g_nat9_spare < 512) { uint64_t p = (uint64_t)at9_get()(); if (!p) break; g_at9_spare[g_nat9_spare++] = p; }
}
static uint64_t slot_host(uint64_t v);
static const uint8_t *layout_ext(const uint8_t *b, uint32_t len, uint32_t tag, uint64_t *out_len);
/* At the consistent point. */
static void ajm_restore(const uint8_t *b, uint32_t len, char *note, size_t nn) {
    uint64_t al = 0; const AjmHdr *h = (const AjmHdr *)layout_ext(b, len, EXT_AJM, &al);
    if (!h || al < sizeof *h || h->magic != 0x4D4A4142u) { _snprintf_s(note, nn, _TRUNCATE, "no audio decoder state in the slot"); return; }
    if (!g_ajm_on) { _snprintf_s(note, nn, _TRUNCATE, "audio decoders NOT restored (ajmstate 0)"); return; }
    static const AjmRec *idx[AJM_CTX_MAX][AJM_NINST + 1];   /* [ctx][inst + 1] */
    memset(idx, 0, sizeof idx);
    for (uint64_t off = sizeof *h; off + sizeof(AjmRec) <= al;) {
        const AjmRec *r = (const AjmRec *)((const uint8_t *)h + off);
        if (r->ctx > 0 && r->ctx < AJM_CTX_MAX && r->inst >= -1 && r->inst < AJM_NINST) idx[r->ctx][r->inst + 1] = r;
        off += sizeof *r + (r->handle ? AT9_SIZE : 0);
    }
    memcpy(g_base + RVA_AJM_BATCHES, h->batches, AJM_BATCH_BYTES);
    int set = 0, cleared = 0, decoders = 0, missing_ctx = 0, no_block = 0, differed = 0;
    for (int c = 1; c < AJM_CTX_MAX; ++c) {
        uint8_t *ctx = ajm_contexts()[c];
        if (!idx[c][0]) continue;
        if (!ctx) { ++missing_ctx; continue; }
        memcpy(ctx, idx[c][0]->data, AJM_CTX_HDR);
        for (int i = 0; i < AJM_NINST; ++i) {
            uint8_t *ci = ctx + AJM_CTX_HDR + (uint64_t)i * AJM_INST;
            uint64_t cur = *(uint64_t *)(ci + AJM_I_HANDLE);
            const AjmRec *r = idx[c][i + 1];
            if (!r) {
                if (*(int32_t *)ci) { if (cur && g_nat9_free < 1024) g_at9_free[g_nat9_free++] = cur; memset(ci, 0, AJM_INST); ++cleared; }
                continue;
            }
            uint64_t hd = 0;
            if (r->handle) {
                hd = cur ? cur : g_nat9_spare ? g_at9_spare[--g_nat9_spare] : 0;
                if (hd) {
                    const uint64_t *s = (const uint64_t *)(r + 1); uint64_t *d = (uint64_t *)hd;
                    for (uint64_t k = 0; k < AT9_SIZE / 8; ++k) { uint64_t v = s[k]; d[k] = v >= r->handle && v < r->handle + AT9_SIZE ? v - r->handle + hd : slot_host(v); }
                    ++decoders;
                } else ++no_block;
            } else if (cur && g_nat9_free < 1024) g_at9_free[g_nat9_free++] = cur;
            if (memcmp(ci + AJM_I_HANDLE + 8, r->data + AJM_I_HANDLE + 8, AJM_INST - AJM_I_HANDLE - 8) || memcmp(ci, r->data, AJM_I_HANDLE)) ++differed;
            memcpy(ci, r->data, AJM_INST); *(uint64_t *)(ci + AJM_I_HANDLE) = hd;
            ++set;
        }
    }
    _snprintf_s(note, nn, _TRUNCATE, "audio decoders: %d instances set (%d had moved on since the save, %d decoder states), %d cleared, batch table restored%s%s",
                set, differed, decoders, cleared, missing_ctx ? "; a decoder context of the slot does not exist now!" : "", no_block ? "; some decoder state had no block!" : "");
}
/* After the game runs again: free the decoder blocks nobody uses now. */
static void ajm_finish(void) {
    void (*rel)(void *) = (void (*)(void *))(g_base + RVA_ATRAC9_RELEASE_HANDLE);
    for (int i = 0; i < g_nat9_free; ++i) rel((void *)g_at9_free[i]);
    g_nat9_free = 0;
}

/* ---- guest threads ---- */
typedef struct {
    DWORD tid; HANDLE h; uint64_t gt, tcb, teb, stackbase, stacklimit, tls_addr, tls_len;
    char name[32]; int is_main, suspended, order;
    DECLSPEC_ALIGN(16) CONTEXT ctx;
} GThread;
static GThread g_thr[MAX_THREADS];
static int g_nthr;
typedef struct { uint64_t lo, hi; } Range;
static Range g_all_stacks[1024]; /* every thread of the process, to keep stacks out of the image regions */
static int g_nall_stacks;

typedef LONG (NTAPI *NtQIT)(HANDLE, int, PVOID, ULONG, PULONG);
typedef struct { LONG ExitStatus; PVOID TebBaseAddress; ULONG_PTR pid, tid; ULONG_PTR Affinity; LONG Priority, BasePriority; } TBI;
static NtQIT g_ntqit;

static void close_threads(void) {
    for (int i = 0; i < g_nthr; ++i) if (g_thr[i].h) CloseHandle(g_thr[i].h);
    g_nthr = 0;
}
/* Live GuestThreads in creation order (the runtime pushes new ones at the head of `threads`). The port's
 * own "bb:..." entries (4 x "bb:hle" stay after a movie, e.g. the new game intro) carry no guest TCB, so a
 * save never holds them; counting them here refused every load in such a run ("area still loading"). */
static int live_guest_threads(uint64_t *out, int cap) {
    uint64_t all[2048]; int n = 0, k = 0;
    __try {
        for (uint8_t *p = *(uint8_t **)(g_base + RVA_THREADS); p && n < 2048; p = *(uint8_t **)(p + GT_NEXT)) all[n++] = (uint64_t)p;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    for (int i = n - 1; i >= 0 && k < cap; --i)
        if (!*(int *)(all[i] + GT_FINISHED) && strncmp((const char *)(all[i] + GT_NAME), "bb:", 3)) out[k++] = all[i];
    return k;
}
/* Finds the OS threads that carry a guest TCB in their TEB TLS slot (runtime_thread.c). Runs before
 * anything is suspended (Toolhelp and OpenThread may use the heap). */
static int enumerate_threads(void) {
    close_threads();
    g_nall_stacks = 0;
    DWORD slot = *(DWORD *)(g_base + RVA_TLS_SLOT);
    if (slot >= 64) { blog("tls_slot %lu unusable", slot); return 0; }
    uint64_t live[1024]; int nlive = live_guest_threads(live, 1024);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te; te.dwSize = sizeof te;
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE h = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
        if (!h) continue;
        TBI bi; memset(&bi, 0, sizeof bi);
        if (g_ntqit(h, 0, &bi, sizeof bi, NULL) || !bi.TebBaseAddress) { CloseHandle(h); continue; }
        uint8_t *teb = (uint8_t *)bi.TebBaseAddress;
        if (g_nall_stacks < 1024) { g_all_stacks[g_nall_stacks].lo = *(uint64_t *)(teb + 0x10); g_all_stacks[g_nall_stacks].hi = *(uint64_t *)(teb + 0x08); ++g_nall_stacks; }
        uint64_t tcb = *(uint64_t *)(teb + 0x1480 + 8 * slot), gt = tcb ? *(uint64_t *)(tcb + 16) : 0;
        int order = -1;
        for (int i = 0; i < nlive && gt; ++i) if (live[i] == gt) order = i;
        if (order < 0 || g_nthr == MAX_THREADS) { CloseHandle(h); continue; }
        GThread *t = &g_thr[g_nthr++];
        memset(t, 0, offsetof(GThread, ctx));
        t->tid = te.th32ThreadID; t->h = h; t->gt = gt; t->tcb = tcb; t->teb = (uint64_t)teb; t->order = order;
        t->stackbase = *(uint64_t *)(teb + 0x08); t->stacklimit = *(uint64_t *)(teb + 0x10);
        memcpy(t->name, (void *)(gt + GT_NAME), 31);
        t->tls_addr = *(uint64_t *)(gt + GT_TLS_BLOCK);
        t->tls_len = *(uint64_t *)(gt + GT_TCB) - t->tls_addr + 256;
        t->is_main = te.th32ThreadID == g_pause_tid && g_state == 2;
    }
    CloseHandle(snap);
    return 1;
}
static void log_thread_state(const char *tag, GThread *t) {
    char rip[160], cand[600]; int n = 0, c = 0;
    describe(t->ctx.Rip, rip, sizeof rip);
    cand[0] = 0;
    for (uint64_t a = t->ctx.Rsp; a + 8 <= t->stackbase && a < t->ctx.Rsp + 0x800 && c < 8; a += 8) {
        uint64_t v = *(uint64_t *)a;
        if (is_code_addr(v)) { char d[96]; describe(v, d, sizeof d); n += _snprintf_s(cand + n, sizeof cand - n, _TRUNCATE, " <%s>", d); ++c; if (n < 0) break; }
    }
    blog("  %s tid=%lu '%s' rip=%s rsp=0x%llx%s", tag, t->tid, t->name, rip, t->ctx.Rsp, cand);
}
static void resume_all(void) {
    for (int i = 0; i < g_nthr; ++i) if (g_thr[i].suspended) { ResumeThread(g_thr[i].h); g_thr[i].suspended = 0; }
}
/* Suspends every guest thread except the stopped one; 1 when all of them sit in an ntdll wait. */
static int quiesce(int verbose) {
    int busy = 0;
    for (int i = 0; i < g_nthr; ++i) {
        GThread *t = &g_thr[i];
        if (t->is_main) continue;
        if (SuspendThread(t->h) == (DWORD)-1) { blog("SuspendThread %lu failed %lu", t->tid, GetLastError()); continue; }
        t->suspended = 1;
        t->ctx.ContextFlags = CONTEXT_FULL;
        if (!GetThreadContext(t->h, &t->ctx)) { blog("GetThreadContext %lu failed %lu", t->tid, GetLastError()); ++busy; continue; }
        if (!wait_name(t->ctx.Rip)) { ++busy; if (verbose >= 0) log_thread_state("busy", t); }
        else if (verbose > 0) log_thread_state("wait", t);
    }
    return busy == 0;
}

/* ---- guest memory regions ---- */
static int read_vmas(Vma *out, int cap) {
    uint64_t n = *(uint64_t *)(g_base + RVA_VMA_COUNT);
    Vma *v = *(Vma **)(g_base + RVA_VMAS);
    if (n > (uint64_t)cap) return -1;
    memcpy(out, v, n * sizeof(Vma));
    return (int)n;
}
static uint8_t *backing(void) { return *(uint8_t **)(g_base + RVA_BACKING_BASE); }
static int overlaps(uint64_t a, uint64_t b, uint64_t c, uint64_t d) { return a < d && c < b; }
static Vma g_vm[MAX_VMAS]; static int g_nvm;
static SRegion g_reg[MAX_VMAS + MAX_IMG]; static int g_nreg;
static uint64_t g_img_bytes;
/* VMAs (kind != 1) through backing_base, then RW committed pages of [0x800000000, low_next)
 * without thread stacks: eboot data/bss and other runtime_low_map blocks. Call after
 * enumerate_threads (stack ranges). */
static int collect_regions(void) {
    g_nvm = read_vmas(g_vm, MAX_VMAS);
    if (g_nvm < 0) return 0;
    uint8_t *bb = backing();
    g_nreg = 0; g_img_bytes = 0;
    for (int i = 0; i < g_nvm; ++i) {
        if (g_vm[i].kind == 1) continue;
        g_reg[g_nreg].addr = g_vm[i].start; g_reg[g_nreg].host = bb + g_vm[i].phys; g_reg[g_nreg].size = g_vm[i].end - g_vm[i].start; ++g_nreg;
    }
    uint64_t end = low_next();
    for (uint64_t a = 0x800000000ull; a < end;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)a, &mbi, sizeof mbi)) break;
        uint64_t lo = (uint64_t)mbi.BaseAddress, hi = lo + mbi.RegionSize;
        if (hi > end) hi = end;
        int rw = mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) && !(mbi.Protect & PAGE_GUARD);
        for (int i = 0; rw && i < g_nall_stacks; ++i) if (overlaps(lo, hi, g_all_stacks[i].lo, g_all_stacks[i].hi)) rw = 0;
        if (rw) {
            if (g_nreg == MAX_VMAS + MAX_IMG) return 0;
            g_reg[g_nreg].addr = lo; g_reg[g_nreg].host = (uint8_t *)lo; g_reg[g_nreg].size = hi - lo; ++g_nreg; g_img_bytes += hi - lo;
        }
        a = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}

/* ---- player ---- */
typedef struct { int ok, hp, hpmax, st; uint64_t ins; float pos[3], yaw; } Player;
static Player read_player(void) {
    Player p; memset(&p, 0, sizeof p);
    __try {
        uint64_t A = *(uint64_t *)0x805543ED0ull;
        if (!A) return p;
        p.ins = *(uint64_t *)A;
        if (!p.ins) return p;
        uint64_t mods = *(uint64_t *)(p.ins + 0x3B0);
        uint64_t dm = *(uint64_t *)(mods + 0x20), phys = *(uint64_t *)(mods + 0x68);
        p.hp = *(int *)(dm + 0xF8); p.hpmax = *(int *)(dm + 0xFC); p.st = *(int *)(dm + 0x134);
        memcpy(p.pos, (void *)(phys + 0x1E0), 12); p.yaw = *(float *)(phys + 0x1D4);
        p.ok = p.hpmax > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { p.ok = 0; }
    return p;
}

/* ---- slot layout blob (process + instance identity for L1 loads) ---- */
typedef struct { uint32_t magic, version, pid, nvma, nthr, pad; uint64_t proc_start; } LayoutHdr;
typedef struct { uint64_t gt, tcb, stackbase, tls_addr, tls_len; uint32_t tid, order; char name[32]; } LThr;
/* Experimental (task #3, item 6): load a slot after a respawn. The location's threads were recreated
 * with the same names; slot threads are matched to current ones by name (and creation order), the
 * slot's GuestThread* values in guest memory are rewritten to the new ones and the TLS data (not
 * the TCB) goes into the new threads' blocks. Off by default ("remap 1"). */
static int g_remap_enabled, g_remap_active, g_nremap;
static struct { uint64_t old_gt, new_gt; int slot_i, cur_i; } g_remap[MAX_THREADS];
static int build_remap(const LThr *t, uint32_t n, int from_live) {
    uint64_t live[1024]; int nl = 0;
    if (from_live) nl = live_guest_threads(live, 1024);
    int ncur = from_live ? nl : g_nthr;
    if (ncur != (int)n) return 0;
    static int used[1024]; memset(used, 0, sizeof used);
    g_nremap = 0;
    for (uint32_t i = 0; i < n; ++i) {
        int best = -1;
        for (int j = 0; j < ncur; ++j) {
            uint64_t gt = from_live ? live[j] : g_thr[j].gt;
            const char *nm = from_live ? (const char *)(gt + GT_NAME) : g_thr[j].name;
            if (used[j] || strncmp(nm, t[i].name, 31)) continue;
            if (gt == t[i].gt) { best = j; break; }
            if (best < 0) best = j;
        }
        if (best < 0) return 0;
        used[best] = 1;
        g_remap[g_nremap].old_gt = t[i].gt; g_remap[g_nremap].new_gt = from_live ? live[best] : g_thr[best].gt;
        g_remap[g_nremap].slot_i = (int)i; g_remap[g_nremap].cur_i = best; ++g_nremap;
    }
    return 1;
}
static uint64_t remap_memory(double *ms) {
    double t0 = now_ms(); uint64_t n = 0;
    uint64_t olds[MAX_THREADS], news[MAX_THREADS]; int k = 0;
    for (int i = 0; i < g_nremap; ++i) if (g_remap[i].old_gt != g_remap[i].new_gt) { olds[k] = g_remap[i].old_gt; news[k] = g_remap[i].new_gt; ++k; }
    if (k) for (int r = 0; r < g_nreg; ++r) {
        uint64_t *p = (uint64_t *)g_reg[r].host, cnt = g_reg[r].size / 8;
        for (uint64_t q = 0; q < cnt; ++q) {
            uint64_t v = p[q];
            if (v < 0x800000000ull || v >= 0x1000000000ull) continue; /* GuestThreads live in the low area */
            for (int i = 0; i < k; ++i) if (v == olds[i]) { p[q] = news[i]; ++n; break; }
        }
    }
    *ms = now_ms() - t0;
    return n;
}
/* TLS blocks (host heap, not guest memory) hold per-thread pointers into the guest heap, e.g. objects
 * a worker creates lazily on first use. A slot saved right after a respawn, before CSChrThread made
 * its object, crashed on load (the restored heap no longer had it, the TLS still pointed at it): so
 * a slot keeps every thread's TLS block and a load puts it back. Stacks are not touched (L2 broke). */
static uint8_t *g_tls; static uint64_t g_tls_len, g_tls_cap = 4ull << 20;
static void tls_capture(void) {
    g_tls_len = 0;
    for (int i = 0; i < g_nthr; ++i) {
        if (g_tls_len + g_thr[i].tls_len > g_tls_cap) break;
        memcpy(g_tls + g_tls_len, (void *)g_thr[i].tls_addr, g_thr[i].tls_len); g_tls_len += g_thr[i].tls_len;
    }
}
static VOState g_vo; /* captured at the save's consistent point */
static FState g_files;
static SemState g_sema;
static const VOState *layout_vo(const uint8_t *b, uint32_t len) {
    const LayoutHdr *h = (const LayoutHdr *)b;
    uint64_t off = sizeof *h + h->nvma * sizeof(Vma) + h->nthr * sizeof(LThr);
    return off + sizeof(VOState) <= len ? (const VOState *)(b + off) : NULL;
}
static const FState *layout_files(const uint8_t *b, uint32_t len) {
    const LayoutHdr *h = (const LayoutHdr *)b;
    uint64_t off = sizeof *h + h->nvma * sizeof(Vma) + h->nthr * sizeof(LThr) + sizeof(VOState);
    return off + sizeof(FState) <= len ? (const FState *)(b + off) : NULL;
}
static const SemState *layout_sema(const uint8_t *b, uint32_t len) {
    const LayoutHdr *h = (const LayoutHdr *)b;
    uint64_t off = sizeof *h + h->nvma * sizeof(Vma) + h->nthr * sizeof(LThr) + sizeof(VOState) + sizeof(FState);
    return off + sizeof(SemState) <= len ? (const SemState *)(b + off) : NULL;
}
/* Format 7: tagged sections after the TLS data (LayoutHdr.pad = their offset): vsync table, host
 * module list, per-thread host TLS, process clock, VideoOut port address (cross-process loads). */
typedef struct { uint32_t tag, pad; uint64_t len; } ExtHdr;
#define EXT_VSYNC 0x4E595356u /* 'VSYN' */
#define MAX_EXT 16
static struct { uint32_t tag; const void *p; uint64_t len; } g_ext[MAX_EXT];
static int g_next;
static uint8_t *g_vsbuf;
static void ext_add(uint32_t tag, const void *p, uint64_t len) { if (g_next < MAX_EXT && p && len) { g_ext[g_next].tag = tag; g_ext[g_next].p = p; g_ext[g_next].len = len; ++g_next; } }
static const uint8_t *layout_ext(const uint8_t *b, uint32_t len, uint32_t tag, uint64_t *out_len) {
    const LayoutHdr *h = (const LayoutHdr *)b;
    if (!b || len < sizeof *h || !h->pad) return NULL;
    for (uint64_t off = h->pad; off + sizeof(ExtHdr) <= len;) {
        const ExtHdr *e = (const ExtHdr *)(b + off);
        if (off + sizeof *e + e->len > len) break;
        if (e->tag == tag) { if (out_len) *out_len = e->len; return (const uint8_t *)(e + 1); }
        off += sizeof *e + ((e->len + 15) & ~15ull);
    }
    return NULL;
}
static uint8_t *build_layout(uint32_t *len) {
    uint64_t base = sizeof(LayoutHdr) + g_nvm * sizeof(Vma) + g_nthr * sizeof(LThr) + sizeof(VOState) + sizeof(FState) + sizeof(SemState) + g_tls_len;
    uint64_t ext = (base + 15) & ~15ull, total = ext;
    for (int i = 0; i < g_next; ++i) total += sizeof(ExtHdr) + ((g_ext[i].len + 15) & ~15ull);
    *len = (uint32_t)total;
    uint8_t *b = (uint8_t *)calloc(1, total);
    LayoutHdr h = {0x594C4242u, FORMAT_VERSION, GetCurrentProcessId(), (uint32_t)g_nvm, (uint32_t)g_nthr, g_next ? (uint32_t)ext : 0, g_proc_start};
    memcpy(b, &h, sizeof h);
    memcpy(b + sizeof h, g_vm, g_nvm * sizeof(Vma));
    LThr *t = (LThr *)(b + sizeof h + g_nvm * sizeof(Vma));
    for (int i = 0; i < g_nthr; ++i) { t[i].gt = g_thr[i].gt; t[i].tcb = g_thr[i].tcb; t[i].stackbase = g_thr[i].stackbase; t[i].tid = g_thr[i].tid; t[i].order = g_thr[i].order; t[i].tls_addr = g_thr[i].tls_addr; t[i].tls_len = g_thr[i].tls_len; memcpy(t[i].name, g_thr[i].name, 32); }
    memcpy(t + g_nthr, &g_vo, sizeof g_vo);
    memcpy((uint8_t *)(t + g_nthr) + sizeof g_vo, &g_files, sizeof g_files);
    memcpy((uint8_t *)(t + g_nthr) + sizeof g_vo + sizeof g_files, &g_sema, sizeof g_sema);
    memcpy((uint8_t *)(t + g_nthr) + sizeof g_vo + sizeof g_files + sizeof g_sema, g_tls, g_tls_len);
    for (int i = 0; i < g_next; ++i) {
        ExtHdr e = {g_ext[i].tag, 0, g_ext[i].len};
        memcpy(b + ext, &e, sizeof e); memcpy(b + ext + sizeof e, g_ext[i].p, g_ext[i].len);
        ext += sizeof e + ((g_ext[i].len + 15) & ~15ull);
    }
    return b;
}
/* Puts back the TLS block of every guest thread (matched by GuestThread*, same address and size). */
static void tls_restore(const uint8_t *b, uint32_t len, char *note, size_t nn) {
    const LayoutHdr *h = (const LayoutHdr *)b;
    const LThr *t = (const LThr *)(b + sizeof *h + h->nvma * sizeof(Vma));
    uint64_t off = sizeof *h + h->nvma * sizeof(Vma) + h->nthr * sizeof(LThr) + sizeof(VOState) + sizeof(FState) + sizeof(SemState);
    int done = 0, changed = 0, skipped = 0;
    for (uint32_t i = 0; i < h->nthr; ++i) {
        if (off + t[i].tls_len > len) { ++skipped; break; }
        int j; for (j = 0; j < g_nthr && g_thr[j].gt != t[i].gt; ++j) {}
        if (g_remap_active) {
            for (int r = 0; r < g_nremap; ++r) if (g_remap[r].slot_i == (int)i) j = g_remap[r].cur_i;
            if (j < g_nthr && g_thr[j].tls_addr != t[i].tls_addr && g_thr[j].tls_len == t[i].tls_len) {
                uint64_t data = t[i].tcb - t[i].tls_addr; /* static TLS only: the TCB (self, dtv, thread) stays */
                memcpy((void *)g_thr[j].tls_addr, b + off, data); ++changed; ++done; off += t[i].tls_len; continue;
            }
        }
        if (j < g_nthr && g_thr[j].tls_addr == t[i].tls_addr && g_thr[j].tls_len == t[i].tls_len) {
            if (memcmp((void *)t[i].tls_addr, b + off, t[i].tls_len)) { memcpy((void *)t[i].tls_addr, b + off, t[i].tls_len); ++changed; }
            ++done;
        } else ++skipped;
        off += t[i].tls_len;
    }
    _snprintf_s(note, nn, _TRUNCATE, "TLS of %d threads checked, %d restored, %d skipped", done, changed, skipped);
}
static int g_l3;
static int g_noinv;    /* debug: no GPU invalidation of the written ranges after the threads resume (command "noinv 1") */
static char g_last_load_note[200];
static int g_verify;   /* debug: compare memory with the slot after the load (command "verifyload 1") */
static int g_xstats;   /* cross-process load: full statistics of host references (slow, one thread); command "xstats 1" */
static int g_l1_stacks = 2;   /* same-process load: 0 stacks as they are, 1 compare with the slot (log), 2 guest frames from the slot (task 5: without them a load
                                * after heavy streaming crashed: locals still pointed at blocks the restored heap has free) */
static int l3_compatible(const uint8_t *b, uint32_t len, char *reason, size_t rlen);
/* Same process and same location instance? reason gets a human readable refusal. A slot of another
 * process goes through l3_compatible (cross-process load) instead of the instance checks. */
static int g_l3_enabled;    /* F9 / load of a slot from another process or location instance (ini cross_process_load, command "l3 1") */
static int g_l3_same;   /* the load goes the L3 way inside this process (the location was reloaded since the save) */
/* The slot's location instance is the one running now: every GuestThread of the slot is alive (same tid
 * at the consistent point). */
static int same_instance(const LThr *t, uint32_t nt, int check_threads) {
    if (check_threads) {
        if (g_nthr != (int)nt) return 0;
        for (int i = 0; i < g_nthr; ++i) { uint32_t j; for (j = 0; j < nt; ++j) if (t[j].gt == g_thr[i].gt && t[j].tid == g_thr[i].tid) break; if (j == nt) return 0; }
        return 1;
    }
    uint64_t live[1024]; int n = live_guest_threads(live, 1024);
    if (n != (int)nt) return 0;
    for (int i = 0; i < n; ++i) { uint32_t j; for (j = 0; j < nt; ++j) if (t[j].gt == live[i]) break; if (j == nt) return 0; }
    return 1;
}
static int layout_matches(const uint8_t *b, uint32_t len, int check_threads, char *reason, size_t rlen) {
    const LayoutHdr *h = (const LayoutHdr *)b;
    if (!b || len < sizeof *h || h->magic != 0x594C4242u) { _snprintf_s(reason, rlen, _TRUNCATE, "slot has no layout.bin"); return 0; }
    if (h->version != FORMAT_VERSION) { _snprintf_s(reason, rlen, _TRUNCATE, "slot format %u, this DLL needs %d (save it again)", h->version, FORMAT_VERSION); return 0; }
    if (!!layout_ext(b, len, EXT_VSYNC, NULL) != !!g_virt) {
        _snprintf_s(reason, rlen, _TRUNCATE, g_virt ? "slot was saved without handle virtualization (the game was started without pdh.dll early mode)" : "slot needs handle virtualization (start the game with pdh.dll installed)"); return 0;
    }
    const LThr *t = (const LThr *)(b + sizeof *h + h->nvma * sizeof(Vma));
    int other_proc = h->pid != GetCurrentProcessId() || h->proc_start != g_proc_start;
    g_l3 = 0; g_l3_same = 0; g_remap_active = 0;
    uint64_t vl = 0; const uint8_t *vs = layout_ext(b, len, EXT_VSYNC, &vl);
    if (!other_proc && (!same_instance(t, h->nthr, check_threads) || (g_virt && vs_slot_table_id(vs, vl) != vs_table_id()))) {
        if (!g_virt || !g_l3_enabled) { _snprintf_s(reason, rlen, _TRUNCATE, "location was reloaded since the save (death/respawn or area load); loading across locations needs pdh.dll early mode and cross_process_load=1"); return 0; }
        g_l3_same = 1;
    }
    if (other_proc || g_l3_same) {
        if (!l3_compatible(b, len, reason, rlen)) return 0;
        g_l3 = 1;
    }
    const Vma *v = (const Vma *)(b + sizeof *h);
    static Vma vm[MAX_VMAS];
    int nv = read_vmas(vm, MAX_VMAS);
    if (nv != (int)h->nvma) { _snprintf_s(reason, rlen, _TRUNCATE, "guest memory layout differs (%d VMAs, slot %u): cannot load here", nv, h->nvma); return 0; }
    for (int i = 0; i < nv; ++i)
        if (vm[i].start != v[i].start || vm[i].end != v[i].end || vm[i].kind != v[i].kind || vm[i].phys != v[i].phys) {
            _snprintf_s(reason, rlen, _TRUNCATE, "guest memory layout differs (VMA %d 0x%llx-0x%llx, slot 0x%llx-0x%llx): cannot load here", i, vm[i].start, vm[i].end, v[i].start, v[i].end); return 0;
        }
    return 1;
}

/* ---- thread blob for L3 research: per thread a record, its used stack and TLS block ---- */
typedef struct {
    char name[32]; uint32_t tid, order, is_main, pad; uint64_t gt, tcb, teb, stackbase, stacklimit, rsp, tls_addr, tls_len, stack_len, dead_len;
    DECLSPEC_ALIGN(16) CONTEXT ctx;
} TRec;   /* followed by the used stack, the TLS block and the dead window (each padded to 16) */
/* Dead window: the DEAD_WIN bytes below a waiting thread's stack pointer, frames of calls that returned.
 * Guest memory keeps stale addresses into them (copies of stack structs, list nodes that pointed at a
 * stack head; ~100k for the main thread, ~10k per GXWorker): a cross-process load translates those like
 * the live part and puts the window's contents back, so the stack below the stack pointer is the slot's
 * too. Not for the render thread stopped in the frame hook (below its frame are this DLL's frames). */
#define DEAD_WIN 0x10000ull
static int g_deadwin = 1;   /* debug: command "deadwin 0" loads do not write the dead window back */
static uint64_t dead_lo(const GThread *t) {
    if (t->is_main) return g_pause_frame;
    uint64_t lo = t->ctx.Rsp > DEAD_WIN ? (t->ctx.Rsp - DEAD_WIN) & ~7ull : 0;
    return lo < t->stacklimit ? t->stacklimit : lo;
}
static uint8_t *g_tblob; static uint64_t g_tblob_cap = 16ull << 20, g_tblob_len;
static void capture_threads(void) {
    uint64_t off = 0;
    for (int i = 0; i < g_nthr; ++i) {
        GThread *t = &g_thr[i];
        uint64_t rsp = t->is_main ? g_pause_frame : t->ctx.Rsp, sl = t->stackbase > rsp ? t->stackbase - rsp : 0, dl = rsp - dead_lo(t);
        if (off + sizeof(TRec) + sl + t->tls_len + dl + 48 > g_tblob_cap) break;
        TRec *r = (TRec *)(g_tblob + off);
        memset(r, 0, sizeof *r);
        memcpy(r->name, t->name, 32); r->tid = t->tid; r->order = t->order; r->is_main = t->is_main;
        r->gt = t->gt; r->tcb = t->tcb; r->teb = t->teb; r->stackbase = t->stackbase; r->stacklimit = t->stacklimit; r->rsp = rsp;
        r->tls_addr = t->tls_addr; r->tls_len = t->tls_len; r->stack_len = sl; r->dead_len = dl;
        if (!t->is_main) r->ctx = t->ctx;
        off += sizeof(TRec);
        memcpy(g_tblob + off, (void *)rsp, sl); off += (sl + 15) & ~15ull;
        memcpy(g_tblob + off, (void *)t->tls_addr, t->tls_len); off += (t->tls_len + 15) & ~15ull;
        memcpy(g_tblob + off, (void *)(rsp - dl), dl); off += (dl + 15) & ~15ull;
    }
    g_tblob_len = off;
}
/* host objects: tracked live ones (created after injection) and kept ones */
typedef struct { uint64_t key, where; uint32_t seq, type, gen, kept; } ORec;
static uint8_t *g_oblob; static uint64_t g_oblob_cap, g_oblob_len;
static void capture_objects(void) {
    uint64_t n = 0;
    ORec *o = (ORec *)(g_oblob + 16);
    uint64_t cap = (g_oblob_cap - 16) / sizeof(ORec);
    AcquireSRWLockShared(&g_tab_lock);
    for (uint32_t i = 0; i < TAB_SIZE && n < cap; ++i)
        if (g_tab[i].key) { o[n].key = g_tab[i].key; o[n].where = g_tab[i].where; o[n].seq = g_tab[i].seq; o[n].type = g_tab[i].type; o[n].gen = g_tab[i].gen; o[n].kept = 0; ++n; }
    ReleaseSRWLockShared(&g_tab_lock);
    LONG nk = g_nkept < KEPT_CAP ? g_nkept : KEPT_CAP;
    for (LONG i = 0; i < nk && n < cap; ++i) { o[n].key = g_keptlist[i].key; o[n].where = g_keptlist[i].where; o[n].seq = g_keptlist[i].seq; o[n].type = g_keptlist[i].type; o[n].gen = 0; o[n].kept = 1; ++n; }
    uint32_t h[4] = {0x4A424F42u, FORMAT_VERSION, (uint32_t)n, (uint32_t)sizeof(ORec)};
    memcpy(g_oblob, h, 16);
    g_oblob_len = 16 + n * sizeof(ORec);
}

/* ---- metadata.json ---- */
typedef struct { char *b; size_t n, cap; } Buf;
static void jb(Buf *b, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int m = _vsnprintf_s(b->b + b->n, b->cap - b->n, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (m >= 0) { b->n += m; return; }
        b->cap = b->cap * 2 + 4096; b->b = (char *)realloc(b->b, b->cap);
    }
}
static char *build_meta(int slot, const Player *pl, const SaveStats *st, double pause_ms) {
    Buf b = {(char *)malloc(65536), 0, 65536};
    SYSTEMTIME t; GetLocalTime(&t);
    char esc[64]; int k;
    jb(&b, "{\n  \"format\": %d,\n  \"complete\": false,\n  \"slot\": %d,\n  \"created\": \"%04d-%02d-%02dT%02d:%02d:%02d\",\n", FORMAT_VERSION, slot, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    jb(&b, "  \"exe_sha256\": \"%s\",\n  \"process\": {\"pid\": %lu, \"start_filetime\": %llu},\n", g_exe_sha, GetCurrentProcessId(), g_proc_start);
    jb(&b, "  \"level\": \"L1\",\n  \"note\": \"L1 load needs the same process and the same location instance (VMAs + guest threads, layout.bin)\",\n");
    jb(&b, "  \"player\": {\"hp\": %d, \"hp_max\": %d, \"stamina\": %d, \"pos\": [%.4f, %.4f, %.4f], \"yaw\": %.5f, \"PlayerIns\": \"0x%llx\"},\n",
       pl->hp, pl->hpmax, pl->st, pl->pos[0], pl->pos[1], pl->pos[2], pl->yaw, pl->ins);
    jb(&b, "  \"fallback\": {\"save_files\": \"fallback/ (userdata0001, backup0001, userdata0010, backup0010 at save time)\", \"teleport\": {\"pos\": [%.4f, %.4f, %.4f], \"yaw\": %.5f, \"hp\": %d}},\n",
       pl->pos[0], pl->pos[1], pl->pos[2], pl->yaw, pl->hp);
    jb(&b, "  \"stats\": {\"pages\": %llu, \"zero_pages\": %llu, \"pages_already_in_pool\": %llu, \"new_pages\": %llu, \"pause_ms\": %.1f, \"hash_ms\": %.1f, \"copy_ms\": %.1f},\n",
       st->pages, st->zero, st->known, st->staged, pause_ms, st->hash_ms, st->copy_ms);
    jb(&b, "  \"vmas\": [\n");
    for (int i = 0; i < g_nvm; ++i)
        jb(&b, "    {\"start\": \"0x%llx\", \"end\": \"0x%llx\", \"kind\": %d, \"prot\": %d, \"type\": %d, \"phys\": \"0x%llx\"}%s\n", g_vm[i].start, g_vm[i].end, g_vm[i].kind, g_vm[i].prot, g_vm[i].type, g_vm[i].phys, i + 1 < g_nvm ? "," : "");
    jb(&b, "  ],\n  \"regions\": [\n");
    for (int i = 0; i < g_nreg; ++i) jb(&b, "    {\"addr\": \"0x%llx\", \"size\": %llu}%s\n", g_reg[i].addr, g_reg[i].size, i + 1 < g_nreg ? "," : "");
    jb(&b, "  ],\n  \"threads\": [\n");
    for (int i = 0; i < g_nthr; ++i) {
        GThread *th = &g_thr[i];
        for (k = 0; th->name[k] && k < 31; ++k) esc[k] = (th->name[k] == '"' || th->name[k] == '\\' || (uint8_t)th->name[k] < 32) ? '_' : th->name[k];
        esc[k] = 0;
        jb(&b, "    {\"order\": %d, \"name\": \"%s\", \"tid\": %lu, \"GuestThread\": \"0x%llx\", \"tcb\": \"0x%llx\", \"tls\": \"0x%llx\", \"tls_len\": %llu, \"stackbase\": \"0x%llx\", \"rsp\": \"0x%llx\", \"stopped_in_hook\": %d}%s\n",
           th->order, esc, th->tid, th->gt, th->tcb, th->tls_addr, th->tls_len, th->stackbase, th->is_main ? g_pause_frame : th->ctx.Rsp, th->is_main, i + 1 < g_nthr ? "," : "");
    }
    jb(&b, "  ],\n  \"host_objects\": {\"file\": \"hostobjs.bin\", \"record\": \"u64 host_handle, u64 guest_slot_address, u32 seq, u32 type, u32 gen, u32 kept\",\n");
    jb(&b, "    \"types\": \"0 mutex, 1 posix mutex, 2 cond, 3 posix cond, 4 mutexattr, 5 posix mutexattr, 6 rwlock, 7 sema id, 8 thread attr\",\n");
    jb(&b, "    \"tracked_live\": %ld, \"kept\": %ld, \"note\": \"only objects created after the DLL was injected are tracked\"},\n", g_tab_count, g_nkept);
    jb(&b, "  \"threads_file\": \"threads.bin: zstd; per thread TRec (bbstate.c) + used stack + TLS block\",\n");
    jb(&b, "  \"pages_file\": \"pages.bin: zstd; u32 'BBPT', u32 1, u32 nregions, u32 0, u64 npages, {u64 addr, u64 npages}[n], XXH3-128 per page ({0,0} = zero page); page data in ../pool\"\n}\n");
    return b.b;
}

static int reach_point(const char *nm, double *t_ready);
/* ---- L3: host references in guest memory (census) and their translation to another process ----
 * Host objects whose addresses the guest keeps: GuestThread (ScePthread), TCB, static TLS block, module
 * TLS blocks (__tls_get_addr), VideoOut port (buffer labels), EqueueInternal, host module images
 * (import slots, return addresses on fiber stacks, &stack_canary, the TCB's dtv). A slot lists them as
 * ranges (XREF) with the host region map (HREG); a load in another process maps each range to the
 * same object there (thread by creation order + name, module by name, equeue by key) and rewrites
 * every aligned qword of guest memory inside one. vsync handles need nothing (same numbers). */
enum { X_GT, X_TCB, X_TLS, X_MTLS, X_VO, X_EQ, X_STACK, X_MOD, X_NKNOWN, X_IMG = X_NKNOWN, X_LOW, X_PRIV, X_MAP, X_NCAT };
static const char *const k_xname[X_NCAT] = {"GuestThread*", "TCB", "static TLS block", "module TLS block", "VideoOut port/labels",
    "EqueueInternal*", "guest thread stack", "host module image", "image (not in module list)", "low-area host alloc", "host heap/private", "mapped file"};
typedef struct { uint64_t lo, hi, to; int32_t cat, idx; char name[40]; uint64_t hits; } XR;   /* 80 B */
typedef struct { uint64_t lo, hi; int32_t kind, pad; } HR;
#define XR_MAX 4096
#define HR_MAX 65536
#define GT_SIZE 416
static XR g_xr[XR_MAX]; static int g_nxr;
static HR g_hr[HR_MAX]; static int g_nhr;
static int cmp_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static int cmp_xr(const void *a, const void *b) { uint64_t x = ((const XR *)a)->lo, y = ((const XR *)b)->lo; return x < y ? -1 : x > y; }
static int cmp_hr(const void *a, const void *b) { uint64_t x = ((const HR *)a)->lo, y = ((const HR *)b)->lo; return x < y ? -1 : x > y; }
static void xr_add(uint64_t lo, uint64_t hi, int cat, int idx, const char *name) {
    if (g_nxr == XR_MAX || hi <= lo) return;
    XR *r = &g_xr[g_nxr++]; memset(r, 0, sizeof *r);
    r->lo = lo; r->hi = hi; r->cat = cat; r->idx = idx;
    if (name) strncpy_s(r->name, sizeof r->name, name, _TRUNCATE);
}
/* address of a bb-probe thread-local variable (native TLS: TEB+0x58 -> [_tls_index]) of another thread */
static uint8_t *tls_var(uint64_t teb, uint64_t rva) {
    uint64_t *arr = *(uint64_t **)(teb + 0x58);
    if (!arr) return NULL;
    uint8_t *blk = (uint8_t *)arr[*(uint32_t *)(g_base + RVA_TLS_INDEX)];
    return blk ? blk + (rva - RVA_TLS_START) : NULL;
}
typedef struct { uint64_t data, filesz, memsz; } TlsModule;
static const TlsModule *tls_module(int m) { return (const TlsModule *)(g_base + RVA_TLS_MODULES) + m; }
static void refresh_modules(void) { g_nmods = 0; free(g_ntx); g_ntx = NULL; g_nntx = 0; init_modules(); }
/* Known host objects of this process (after enumerate_threads). */
static void build_known(void) {
    g_nxr = 0;
    for (int i = 0; i < g_nthr; ++i) {
        GThread *t = &g_thr[i];
        xr_add(t->gt, t->gt + GT_SIZE, X_GT, t->order, t->name);
        xr_add(t->tcb, t->tcb + 256, X_TCB, t->order, t->name);
        xr_add(t->tls_addr, t->tcb, X_TLS, t->order, t->name);
        /* the live part and the dead window below it (the rest of the stack is old: the process main
         * thread's stack sits at small addresses that plain integers in guest memory hit) */
        xr_add(dead_lo(t), t->stackbase, X_STACK, t->order, t->name);
        uint64_t *blocks = (uint64_t *)tls_var(t->teb, RVA_TLSV_TLS_BLOCKS);
        for (int m = 2; blocks && m < 8; ++m) if (blocks[m]) xr_add(blocks[m], blocks[m] + tls_module(m)->memsz, X_MTLS, t->order * 8 + m, t->name);
    }
    uint8_t *port = vo_port();
    if (port) xr_add((uint64_t)port, (uint64_t)port + 0x400, X_VO, 0, "port");
    int32_t eqk[16]; uint8_t *eqs[16]; int neq = vo_equeues(eqk, eqs, 16);
    for (int i = 0; i < neq; ++i) xr_add((uint64_t)eqs[i], (uint64_t)eqs[i] + 0x80, X_EQ, eqk[i], "equeue");
    for (int k = 0; k < g_nmods; ++k) xr_add(g_mods[k].base, g_mods[k].base + g_mods[k].size, X_MOD, k, g_mods[k].name);
    qsort(g_xr, g_nxr, sizeof *g_xr, cmp_xr);
    for (int i = 1; i < g_nxr; ++i) if (g_xr[i].lo < g_xr[i - 1].hi) blog("census: ranges overlap: %s 0x%llx-0x%llx and %s 0x%llx-0x%llx", k_xname[g_xr[i - 1].cat], g_xr[i - 1].lo, g_xr[i - 1].hi, k_xname[g_xr[i].cat], g_xr[i].lo, g_xr[i].hi);
}
/* Committed host memory outside guest memory (VMAs, backing view, eboot image). */
static void build_hostmap(void) {
    static Vma vm[MAX_VMAS]; int nv = read_vmas(vm, MAX_VMAS);
    uint64_t bb = (uint64_t)backing(), bb_end = bb;
    for (int i = 0; i < nv; ++i) if (bb + vm[i].phys + (vm[i].end - vm[i].start) > bb_end) bb_end = bb + vm[i].phys + (vm[i].end - vm[i].start);
    g_nhr = 0;
    for (uint64_t a = 0x10000; a < 0x7FFFFFFF0000ull && g_nhr < HR_MAX;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void *)a, &m, sizeof m)) break;
        uint64_t lo = (uint64_t)m.BaseAddress, hi = lo + m.RegionSize;
        int guest = overlaps(lo, hi, bb, bb_end) || overlaps(lo, hi, 0x800000000ull, 0x805700000ull);
        for (int i = 0; i < nv && !guest; ++i) if (overlaps(lo, hi, vm[i].start, vm[i].end)) guest = 1;
        if (m.State == MEM_COMMIT && !guest) {
            HR *r = &g_hr[g_nhr++]; r->lo = lo; r->hi = hi; r->pad = 0;
            r->kind = m.Type == MEM_IMAGE ? X_IMG : m.Type == MEM_MAPPED ? X_MAP : (lo >= 0x800000000ull && lo < low_next()) ? X_LOW : X_PRIV;
        }
        a = hi;
    }
    qsort(g_hr, g_nhr, sizeof *g_hr, cmp_hr);
}
typedef struct {
    uint64_t cnt[X_NCAT], exact[X_NCAT], sample[X_NCAT][3]; int ns[X_NCAT];
    uint64_t handles[VS_NTYPES], dead[VS_NTYPES], translated;
    uint64_t vma[MAX_VMAS][3];   /* per VMA: known objects, modules, other host */
} XStats;
static XStats g_xs;
/* Every aligned qword of g_reg: count (and with translate, rewrite) host references. */
static double xscan(int translate, XStats *st) {
    double t0 = now_ms();
    memset(st, 0, sizeof *st);
    for (int i = 0; i < g_nxr; ++i) g_xr[i].hits = 0;
    uint64_t xlo = g_nxr ? g_xr[0].lo : 0, xhi = 0;
    for (int i = 0; i < g_nxr; ++i) if (g_xr[i].hi > xhi) xhi = g_xr[i].hi;
    for (int r = 0; r < g_nreg; ++r) {
        uint64_t *p = (uint64_t *)g_reg[r].host, n = g_reg[r].size / 8;
        int vi = -1;
        for (int i = 0; i < g_nvm; ++i) if (g_reg[r].addr >= g_vm[i].start && g_reg[r].addr < g_vm[i].end) { vi = i; break; }
        for (uint64_t k = 0; k < n; ++k) {
            uint64_t v = p[k];
            if (v < 0x10000) continue;
            if ((v & VS_TAGMASK) == VS_TAG) { int s = vs_handle_state(v); if (s > 0) ++st->handles[s]; else if (s < 0) ++st->dead[-s]; continue; }
            if (v >= 0x7FFFFFFF0000ull) continue;
            int c = -1, f = -1;
            if (v >= xlo && v < xhi) {
                int lo = 0, hi = g_nxr - 1;
                while (lo <= hi) { int mid = (lo + hi) / 2; if (g_xr[mid].lo <= v) { f = mid; lo = mid + 1; } else hi = mid - 1; }
                if (f >= 0 && v < g_xr[f].hi) {
                    c = g_xr[f].cat; ++g_xr[f].hits;
                    if (v == g_xr[f].lo) ++st->exact[c];
                    if (translate && g_xr[f].to && !(c == X_STACK && (v & 7))) { p[k] = v - g_xr[f].lo + g_xr[f].to; ++st->translated; }   /* stack objects are aligned; odd values there are numbers */
                }
            }
            if (c < 0) {
                int lo = 0, hi = g_nhr - 1; f = -1;
                while (lo <= hi) { int mid = (lo + hi) / 2; if (g_hr[mid].lo <= v) { f = mid; lo = mid + 1; } else hi = mid - 1; }
                if (f < 0 || v >= g_hr[f].hi) continue;
                c = g_hr[f].kind;
            }
            ++st->cnt[c];
            if (st->ns[c] < 3) st->sample[c][st->ns[c]++] = g_reg[r].addr + k * 8;
            if (vi >= 0) ++st->vma[vi][c < X_MOD ? 0 : c == X_MOD || c == X_IMG ? 1 : 2];
        }
    }
    return now_ms() - t0;
}
/* The translation of a load on all fast threads: 1 MB pieces of g_reg, only rewriting (no statistics). */
#define XP_CHUNK (1ull << 20)
static uint64_t g_xp_first[MAX_VMAS + MAX_IMG + 1], g_xp_items;
static volatile LONG64 g_xp_done[64];
static void xlate_buf(uint64_t *p, uint64_t n, uint64_t *count);
static void xpar_task(void *ctx, uint64_t item, int t) {
    (void)ctx;
    int lo = 0, hi = g_nreg - 1, r = 0;
    while (lo <= hi) { int mid = (lo + hi) / 2; if (g_xp_first[mid] <= item) { r = mid; lo = mid + 1; } else hi = mid - 1; }
    uint64_t off = (item - g_xp_first[r]) * XP_CHUNK, len = g_reg[r].size - off;
    if (len > XP_CHUNK) len = XP_CHUNK;
    uint64_t n = 0;
    xlate_buf((uint64_t *)(g_reg[r].host + off), len / 8, &n);
    g_xp_done[t & 63] += n;
}
static double xscan_parallel(uint64_t *translated) {
    double t0 = now_ms();
    g_xp_items = 0;
    for (int r = 0; r < g_nreg; ++r) { g_xp_first[r] = g_xp_items; g_xp_items += (g_reg[r].size + XP_CHUNK - 1) / XP_CHUNK; }
    memset((void *)g_xp_done, 0, sizeof g_xp_done);
    store_parallel(xpar_task, NULL, g_xp_items);
    *translated = 0;
    for (int t = 0; t < 64; ++t) *translated += g_xp_done[t];
    return now_ms() - t0;
}
static void xstats_log(const char *tag, const XStats *st, double ms) {
    blog("%s: scan %.0f ms over %d regions; %d known host ranges, %d host regions", tag, ms, g_nreg, g_nxr, g_nhr);
    for (int c = 0; c < X_NCAT; ++c)
        if (st->cnt[c]) blog("  %-28s %8llu (exactly the object's address: %llu)  e.g. at 0x%llx 0x%llx 0x%llx", k_xname[c], st->cnt[c], st->exact[c],
                             st->ns[c] > 0 ? st->sample[c][0] : 0, st->ns[c] > 1 ? st->sample[c][1] : 0, st->ns[c] > 2 ? st->sample[c][2] : 0);
    blog("  vsync handles: mutex %llu, cond %llu, mutex attr %llu, rwlock %llu; with the tag but no live object: %llu/%llu/%llu/%llu",
         st->handles[VS_MUTEX], st->handles[VS_COND], st->handles[VS_MATTR], st->handles[VS_RWLOCK], st->dead[VS_MUTEX], st->dead[VS_COND], st->dead[VS_MATTR], st->dead[VS_RWLOCK]);
    if (st->translated) blog("  rewritten: %llu", st->translated);
    for (int pass = 0; pass < 6; ++pass) {   /* busiest stack ranges */
        int best = -1;
        for (int i = 0; i < g_nxr; ++i) if (g_xr[i].cat == X_STACK && g_xr[i].hits && (best < 0 || g_xr[i].hits > g_xr[best].hits)) best = i;
        if (best < 0) break;
        /* what the values are: distinct targets, alignment */
        static uint64_t tv[4096], tc[4096]; int nt = 0; uint64_t aligned = 0, total = 0, overflow = 0;
        for (int r = 0; r < g_nreg; ++r) {
            uint64_t *q = (uint64_t *)g_reg[r].host, nq = g_reg[r].size / 8;
            for (uint64_t k = 0; k < nq; ++k) {
                uint64_t v = q[k];
                if (v < g_xr[best].lo || v >= g_xr[best].hi) continue;
                ++total; aligned += !(v & 7);
                int j; for (j = 0; j < nt && tv[j] != v; ++j) {}
                if (j < nt) ++tc[j]; else if (nt < 4096) { tv[nt] = v; tc[nt] = 1; ++nt; } else ++overflow;
            }
        }
        char top[300]; int w = 0; top[0] = 0;
        for (int pass2 = 0; pass2 < 4; ++pass2) { int bj = -1; for (int j = 0; j < nt; ++j) if (tc[j] && (bj < 0 || tc[j] > tc[bj])) bj = j; if (bj < 0) break; w += _snprintf_s(top + w, sizeof top - w, _TRUNCATE, " 0x%llx x%llu", tv[bj], tc[bj]); tc[bj] = 0; }
        blog("  stack '%s' 0x%llx-0x%llx: %llu values point into it (%llu 8-aligned, %d+ distinct, most:%s)", g_xr[best].name, g_xr[best].lo, g_xr[best].hi, total, aligned, nt + (overflow ? 1 : 0), top);
        g_xr[best].hits = 0;
    }
    for (int i = 0; i < g_nvm; ++i) if (st->vma[i][0] + st->vma[i][1] + st->vma[i][2])
        blog("  VMA 0x%llx-0x%llx: known objects %llu, module addresses %llu, other host %llu", g_vm[i].start, g_vm[i].end, st->vma[i][0], st->vma[i][1], st->vma[i][2]);
}
/* Live module addresses on suspended fibers? A fiber's saved stack pointer lives in guest memory
 * outside the fiber stacks; a module address above such a pointer (within 64 KB) may be a live frame. */
static void fiber_check(void) {
    uint64_t lo = 0, hi = 0;
    for (int i = 0; i < g_nvm; ++i) if (g_vm[i].start == 0x1042c00000ull) { lo = g_vm[i].start; hi = g_vm[i].end; }
    if (!lo) { blog("fibers: VMA 0x1042c00000 not found"); return; }
    static uint64_t sp[200000]; int nsp = 0;
    for (int r = 0; r < g_nreg; ++r) {
        if (g_reg[r].addr >= lo && g_reg[r].addr < hi) continue;
        uint64_t *p = (uint64_t *)g_reg[r].host, n = g_reg[r].size / 8;
        for (uint64_t k = 0; k < n && nsp < 200000; ++k) if (p[k] >= lo && p[k] < hi && !(p[k] & 7)) sp[nsp++] = p[k];
    }
    qsort(sp, nsp, 8, cmp_u64);
    uint64_t mods = 0, nlive = 0, nearest_min = ~0ull, ex[6]; int nex = 0;
    for (int r = 0; r < g_nreg; ++r) {
        if (g_reg[r].addr < lo || g_reg[r].addr >= hi) continue;
        uint64_t *p = (uint64_t *)g_reg[r].host, n = g_reg[r].size / 8;
        for (uint64_t k = 0; k < n; ++k) {
            uint64_t v = p[k], a = g_reg[r].addr + k * 8;
            int m = 0;
            for (int j = 0; j < g_nmods && !m; ++j) if (v >= g_mods[j].base + 0x1000 && v < g_mods[j].base + g_mods[j].size) m = 1;
            if (!m) continue;
            ++mods;
            int l2 = 0, h2 = nsp - 1, f = -1;   /* largest external pointer <= a */
            while (l2 <= h2) { int mid = (l2 + h2) / 2; if (sp[mid] <= a) { f = mid; l2 = mid + 1; } else h2 = mid - 1; }
            if (f >= 0 && a - sp[f] < 0x10000) { ++nlive; if (a - sp[f] < nearest_min) nearest_min = a - sp[f]; if (nex < 6) ex[nex++] = a; }
        }
    }
    blog("fibers: %d pointers into the fiber stacks from elsewhere; %llu module addresses on fiber stacks, %llu of them within 64 KB above such a pointer (closest +0x%llx)",
         nsp, mods, nlive, nearest_min == ~0ull ? 0 : nearest_min);
    for (int i = 0; i < nex; ++i) {
        uint64_t off = ex[i] - lo, v = 0;
        for (int r = 0; r < g_nreg; ++r) if (ex[i] >= g_reg[r].addr && ex[i] < g_reg[r].addr + g_reg[r].size) v = *(uint64_t *)(g_reg[r].host + (ex[i] - g_reg[r].addr));
        char d[120]; describe(v, d, sizeof d);
        blog("  e.g. fiber VMA+0x%llx: %s", off, d);
    }
}
/* Task 6 audit: aligned values in guest memory that point into any thread stack of the process (guest
 * threads: live part above the stack pointer and dead part below it; other host threads: whole committed
 * stack). Each guest-thread hit is logged with the frame it falls into (guest frame-pointer chain). */
static int unwind_to_guest(const CONTEXT *start, CONTEXT *c, uint64_t *regp[16]);
static void find_guest_code(void);
static void stackrefs(void) {
    double t;
    refresh_modules();
    if (!reach_point("STACKREFS", &t)) return;
    if (!collect_regions()) { resume_all(); release_main(); return; }
    find_guest_code();
    typedef struct { uint64_t lo, hi, rsp; int ti; uint64_t live_a, live_u, dead_a, dead_u, near_a, near_d; int nfr, log_l, log_d; uint64_t fr[256]; } SR;
    static SR sr[1024]; int ns = 0;
    for (int i = 0; i < g_nthr && ns < 1024; ++i) {
        GThread *g = &g_thr[i]; SR *s = &sr[ns++]; memset(s, 0, sizeof *s);
        s->lo = g->stacklimit; s->hi = g->stackbase; s->rsp = g->is_main ? g_pause_frame : g->ctx.Rsp; s->ti = i;
        uint64_t rbp = 0;
        if (g->is_main) rbp = *(uint64_t *)(g_pause_frame + 216);
        else { CONTEXT c; uint64_t *rp[16]; if (unwind_to_guest(&g->ctx, &c, rp)) rbp = c.Rbp; }
        while (rbp > s->rsp && rbp + 16 <= s->hi && !(rbp & 7) && s->nfr < 256) { s->fr[s->nfr++] = rbp; uint64_t nx = *(uint64_t *)rbp; if (nx <= rbp) break; rbp = nx; }
    }
    for (int i = 0; i < g_nall_stacks && ns < 1024; ++i) {   /* host threads without a guest identity */
        int dup = 0; for (int j = 0; j < g_nthr; ++j) if (g_thr[j].stackbase == g_all_stacks[i].hi) dup = 1;
        if (dup || g_all_stacks[i].hi == __readgsqword(0x08)) continue;
        SR *s = &sr[ns++]; memset(s, 0, sizeof *s); s->lo = g_all_stacks[i].lo; s->hi = g_all_stacks[i].hi; s->rsp = s->lo; s->ti = -1;
    }
    uint64_t logged = 0, host_a = 0, host_u = 0;
    for (int r = 0; r < g_nreg; ++r) {
        uint64_t *p = (uint64_t *)g_reg[r].host, n = g_reg[r].size / 8;
        for (uint64_t k = 0; k < n; ++k) {
            uint64_t v = p[k];
            if (v < 0x10000 || v >= 0x7FFFFFFF0000ull) continue;
            for (int i = 0; i < ns; ++i) {
                SR *s = &sr[i];
                if (v < s->lo || v >= s->hi) continue;
                int al = !(v & 7);
                if (s->ti < 0) { if (al) ++host_a; else ++host_u; break; }
                int live = v >= s->rsp;
                if (live) { if (al) ++s->live_a; else ++s->live_u; } else { if (al) ++s->dead_a; else ++s->dead_u; }
                if (live) { if (al && v - s->rsp < 0x10000) ++s->near_a; } else if (al && s->rsp - v <= 0x10000) ++s->near_d;
                if (al && logged < 1200 && (live ? s->log_l++ : s->log_d++) < 25) {
                    int f = -1; for (int q = 0; q < s->nfr; ++q) if (v < s->fr[q] + 16) { f = q; break; }
                    uint64_t a = g_reg[r].addr + k * 8, nx = k + 1 < n ? p[k + 1] : 0, pv = k ? p[k - 1] : 0;
                    blog("  STACKREF '%s' %s at 0x%llx = 0x%llx (rsp%+lld, frame %d of %d%s) prev 0x%llx next 0x%llx", g_thr[s->ti].name, live ? "live" : "DEAD", a, v,
                         (long long)(v - s->rsp), f, s->nfr, f >= 0 ? "" : live ? ", above the guest frames" : "", pv, nx);
                    ++logged;
                }
                break;
            }
        }
    }
    for (int i = 0; i < ns; ++i) if (sr[i].ti >= 0 && (sr[i].live_a + sr[i].live_u + sr[i].dead_a + sr[i].dead_u))
        blog("STACKREFS '%s' stack 0x%llx-0x%llx rsp 0x%llx (%d guest frames): live %llu aligned + %llu unaligned, dead (below rsp) %llu aligned + %llu unaligned; aligned within 64 KB below rsp %llu",
             g_thr[sr[i].ti].name, sr[i].lo, sr[i].hi, sr[i].rsp, sr[i].nfr, sr[i].live_a, sr[i].live_u, sr[i].dead_a, sr[i].dead_u, sr[i].near_d);
    blog("STACKREFS: other host thread stacks: %llu aligned + %llu unaligned values point into them; %d stacks checked", host_a, host_u, ns);
    resume_all(); release_main();
}
static void census(void) {
    double t;
    refresh_modules();
    if (!reach_point("CENSUS", &t)) return;
    if (!collect_regions()) { resume_all(); release_main(); blog("census: too many regions"); return; }
    build_known(); build_hostmap();
    double ms = xscan(0, &g_xs);
    xstats_log("CENSUS", &g_xs, ms);
    fiber_check();
    resume_all(); release_main();
}

/* ---- L3 slot data and the cross-process load ---- */
#define EXT_XREF 0x46455258u /* 'XREF' known host ranges of the saving process */
#define EXT_HREG 0x47455248u /* 'HREG' its host region map */
#define EXT_HTLS 0x534C5448u /* 'HTLS' per-thread host thread-locals: pthread keys, errno, module TLS */
#define EXT_INFO 0x4F464E49u /* 'INFO' L3Info */
#define EXT_ASCQ 0x51435341u /* 'ASCQ' GPU compute queues: offset of the last submission per ring (host) */
#define EXT_APPR 0x52505041u /* 'APPR' float: seconds between the player's appearance (start, respawn, travel) and the save */
/* Task 6: a slot saved about 1 s after the player appeared (the area still streaming in behind the fade-in)
 * loads fine in the same run but crashed in another run (2 slots: 5 of 6 and 3 of 3 loads, memory
 * corruption in different places); measured from the PlayerIns appearance those were saved 5.2 s after it,
 * slots saved 4.2, 6.3, 7.3 s after it loaded fine (0 of 10 failed). The cause is not found:
 * such a slot loads only in the run that saved it, another run refuses it with the reason.
 * Task 6.1: not reproduced (slots 4.3-10.5 s after it: 18 of 18 cross-process loads fine, nothing of the
 * host in flight at the consistent point); with fixed stack addresses values that only look like stack
 * addresses are no longer translated at all. The refusal stays as an option (refuse_early_slots=1). */
#define EARLY_SAVE_S 8.0f
static int g_early_ok = 1;   /* task 6.1: such slots load in other runs too (bbstate.ini refuse_early_slots=1 or command "earlyok 0": refused) */
#define ASC_RINGS 56          /* Liverpool::NumComputeRings */
typedef struct {
    char exe_sha[65], pad[3];
    uint32_t dll_stamp, dll_size;
    uint64_t ptime_us;          /* runtime process_time() at the consistent point */
    int64_t realtime_ms;
    uint32_t nthreads, nfiles;
    uint64_t tsc_guest;         /* the guest's TSC and monotonic clock at the consistent point */
    int64_t mono_guest_ns;
    uint32_t canon, pad2;       /* the saving run had the canonical stack depth */
} L3Info;
typedef struct { uint32_t order, pad; char name[32]; uint64_t keys[256]; int32_t err, pad2; uint64_t mtls[8], mlen[8];
                 int32_t wkind, pad3; uint64_t wc1, wc2, site; } HTlsRec; /* + module TLS data. wkind/wc1/wc2: vsync wait; site: first guest return address on the stack */
static L3Info g_l3info;
static uint8_t *g_htls; static uint64_t g_htls_len, g_htls_cap = 8ull << 20;
static uint64_t rt_ptime(void) { return (uint64_t)sysv_call2(g_base + RVA_PROCESS_TIME, 0, 0); }
static void dll_identity(uint32_t *stamp, uint32_t *size) {
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)((uint8_t *)g_self + ((IMAGE_DOS_HEADER *)g_self)->e_lfanew);
    *stamp = nt->FileHeader.TimeDateStamp; *size = nt->OptionalHeader.SizeOfImage;
}
static int is_self_module(const char *name) { return !_strnicmp(name, "bbstate", 7); }
/* Where a thread waits: its vsync wait (cells) and the first guest return address on its stack. */
static void thread_wait(GThread *t, int32_t *kind, uint64_t *c1, uint64_t *c2, uint64_t *site) {
    *kind = vs_thread_wait(t->tid, c1, c2);
    *site = 0;
    uint64_t rsp = t->is_main ? g_pause_frame : t->ctx.Rsp;
    for (uint64_t a = rsp & ~7ull; a + 8 <= t->stackbase && a < rsp + 0x4000; a += 8) {
        uint64_t v = *(uint64_t *)a;
        if (v >= 0x800000000ull && v < 0x8050da000ull) { *site = v; break; }
    }
}
static void capture_htls(void) {
    g_htls_len = 0;
    for (int i = 0; i < g_nthr; ++i) {
        GThread *t = &g_thr[i];
        if (g_htls_len + sizeof(HTlsRec) > g_htls_cap) break;
        HTlsRec *r = (HTlsRec *)(g_htls + g_htls_len);
        memset(r, 0, sizeof *r);
        r->order = (uint32_t)t->order; memcpy(r->name, t->name, 32);
        uint64_t *keys = (uint64_t *)tls_var(t->teb, RVA_TLSV_KEY_VALUES), *blocks = (uint64_t *)tls_var(t->teb, RVA_TLSV_TLS_BLOCKS);
        int32_t *err = (int32_t *)tls_var(t->teb, RVA_TLSV_GUEST_ERRNO);
        if (keys) memcpy(r->keys, keys, sizeof r->keys);
        if (err) r->err = *err;
        thread_wait(t, &r->wkind, &r->wc1, &r->wc2, &r->site);
        uint64_t off = g_htls_len + sizeof *r;
        for (int m = 2; blocks && m < 8; ++m) {
            uint64_t len = tls_module(m)->memsz;
            if (!blocks[m] || off + len > g_htls_cap) continue;
            r->mtls[m] = blocks[m]; r->mlen[m] = len;
            memcpy(g_htls + off, (void *)blocks[m], len); off += (len + 15) & ~15ull;
        }
        g_htls_len = off;
    }
}
/* At the save's consistent point (modules refreshed before): everything a load in another process needs. */
static void capture_l3(void) {
    build_known(); build_hostmap(); capture_htls();
    memset(&g_l3info, 0, sizeof g_l3info);
    memcpy(g_l3info.exe_sha, g_exe_sha, 65);
    dll_identity(&g_l3info.dll_stamp, &g_l3info.dll_size);
    g_l3info.ptime_us = rt_ptime();
    FILETIME f; GetSystemTimeAsFileTime(&f);
    g_l3info.realtime_ms = (int64_t)((((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) - 116444736000000000ull) / 10000;
    g_l3info.nthreads = (uint32_t)g_nthr; g_l3info.nfiles = g_files.n;
    g_l3info.tsc_guest = vc_tsc_c(); g_l3info.mono_guest_ns = mono_guest_ns(); g_l3info.canon = (uint32_t)g_canon;
    ext_add(EXT_INFO, &g_l3info, sizeof g_l3info);
    ext_add(EXT_XREF, g_xr, (uint64_t)g_nxr * sizeof(XR));
    ext_add(EXT_HREG, g_hr, (uint64_t)g_nhr * sizeof(HR));
    ext_add(EXT_HTLS, g_htls, g_htls_len);
}
/* Game files open now vs at save: the location check of a cross-process load. */
/* the location = the map files (sound banks and the like open and close on their own; a load reopens them) */
static int map_file(const char *p) { return game_file(p) && strstr(p, "/map/") != NULL; }
static int same_location(const FState *s, char *reason, size_t rlen) {
    if (!s || s->magic != 0x4C464242u) { _snprintf_s(reason, rlen, _TRUNCATE, "slot has no file table"); return 0; }
    int now = 0;
    for (int fd = 3; fd < FILE_MAX; ++fd) if (file_used(fd) && map_file(file_path(fd))) ++now;
    int saved = 0;
    for (uint32_t i = 0; i < s->n; ++i) {
        if (!map_file(s->f[i].path)) continue;
        ++saved;
        int found = 0;
        for (int fd = 3; fd < FILE_MAX && !found; ++fd) found = file_used(fd) && !strcmp(file_path(fd), s->f[i].path);
        if (!found) { _snprintf_s(reason, rlen, _TRUNCATE, "different location: %s was open at save and is not now (%d map files open, slot %d)", s->f[i].path, now, saved); return 0; }
    }
    if (now != saved) { _snprintf_s(reason, rlen, _TRUNCATE, "different location: %d map files open, slot %d", now, saved); return 0; }
    return 1;
}
static int same_pool(const char *a, const char *b);
static int g_l3_other_loc; static char g_l3_loc_note[300];   /* the slot's map files are not the ones open now */
/* Cross-process checks (no game pause): same exe, same DLL build, virtualization, threads by creation
 * order and name, location (open game files). VMAs are compared by the caller. */
static int l3_compatible(const uint8_t *b, uint32_t len, char *reason, size_t rlen) {
    const LayoutHdr *h = (const LayoutHdr *)b;
    uint64_t il = 0; const L3Info *in = (const L3Info *)layout_ext(b, len, EXT_INFO, &il);
    if (!g_l3_enabled) { _snprintf_s(reason, rlen, _TRUNCATE, "slot was saved in another game process (pid %u); cross-process loading is off (bbstate.ini cross_process_load=1)", h->pid); return 0; }
    if (!in || il != sizeof *in || !layout_ext(b, len, EXT_XREF, NULL)) { _snprintf_s(reason, rlen, _TRUNCATE, "slot from another process has no L3 data (saved by an older DLL or without early mode)"); return 0; }
    if (strcmp(in->exe_sha, g_exe_sha)) { _snprintf_s(reason, rlen, _TRUNCATE, "slot was saved with another bb-probe.exe (sha256 %.16s...)", in->exe_sha); return 0; }
    uint64_t apl = 0; const float *ap = (const float *)layout_ext(b, len, EXT_APPR, &apl);
    if (ap && apl == sizeof *ap && *ap < EARLY_SAVE_S && !g_early_ok) {
        _snprintf_s(reason, rlen, _TRUNCATE, "slot was saved %.1f s after the player appeared, while the area was still loading: it loads only in the game run that saved it", *ap); return 0;
    }
    if (!in->canon != !g_canon) { _snprintf_s(reason, rlen, _TRUNCATE, in->canon ? "this run has no canonical stack depth (DLL not loaded early?)" : "slot was saved without the canonical stack depth (save it again)"); return 0; }
    const LThr *t = (const LThr *)(b + sizeof *h + h->nvma * sizeof(Vma));
    uint64_t live[1024]; int n = live_guest_threads(live, 1024);
    /* the same threads by pool (name without the number) and count; their order may differ (a location
     * reload recreates its threads last) */
    for (uint32_t i = 0; i < h->nthr; ++i) {
        int in_slot = 0, now = 0;
        for (uint32_t j = 0; j < h->nthr; ++j) in_slot += same_pool(t[j].name, t[i].name);
        for (int j = 0; j < n; ++j) now += same_pool((const char *)(live[j] + GT_NAME), t[i].name);
        if (in_slot != now) { _snprintf_s(reason, rlen, _TRUNCATE, "different thread set: %d threads like '%s' in the slot, %d now (%d guest threads now, slot %u; loading screen?)", in_slot, t[i].name, now, n, h->nthr); return 0; }
    }
    if (n != (int)h->nthr) { _snprintf_s(reason, rlen, _TRUNCATE, "different thread set: %d guest threads now, slot %u (loading screen?)", n, h->nthr); return 0; }
    char where[300];
    g_l3_other_loc = !same_location(layout_files(b, len), where, sizeof where);
    if (g_l3_other_loc) strcpy_s(g_l3_loc_note, sizeof g_l3_loc_note, where);
    return 1;
}
static void xlate_buf(uint64_t *p, uint64_t n, uint64_t *count) {
    for (uint64_t k = 0; k < n; ++k) {
        uint64_t v = p[k];
        if (v < 0x10000 || v >= 0x7FFFFFFF0000ull) continue;
        int lo = 0, hi = g_nxr - 1, f = -1;
        while (lo <= hi) { int mid = (lo + hi) / 2; if (g_xr[mid].lo <= v) { f = mid; lo = mid + 1; } else hi = mid - 1; }
        if (f >= 0 && v < g_xr[f].hi && g_xr[f].to && !(g_xr[f].cat == X_STACK && (v & 7))) { p[k] = v - g_xr[f].lo + g_xr[f].to; ++*count; }
    }
}
/* Slot thread (creation order in the saving process) -> thread here. Threads of one pool (same name
 * without the trailing number) are interchangeable: their per-thread objects were allocated in a
 * different order in each run, so a thread here takes the identity of the slot's thread that waited on
 * the same guest cell (vsync), or at the same place; the rest go by name and creation order. */
static GThread *g_l3map[1024];
static const struct HTlsRecS *g_l3rec[1024];
static GThread *thr_by_order(uint32_t order) { return order < 1024 ? g_l3map[order] : NULL; }
static int base_len(const char *n) { int k = (int)strnlen(n, 31); while (k > 0 && n[k - 1] >= '0' && n[k - 1] <= '9') --k; return k; }
static int same_pool(const char *a, const char *b) { int k = base_len(a); return k == base_len(b) && !strncmp(a, b, k); }
static void l3_match(const uint8_t *ht, uint64_t tl, int verbose) {
    memset(g_l3map, 0, sizeof g_l3map); memset(g_l3rec, 0, sizeof g_l3rec);
    static const HTlsRec *rec[1024]; int nrec = 0;
    for (uint64_t o = 0; ht && o + sizeof(HTlsRec) <= tl && nrec < 1024;) {
        const HTlsRec *r = (const HTlsRec *)(ht + o); rec[nrec++] = r; o += sizeof *r;
        if (r->order < 1024) g_l3rec[r->order] = (const struct HTlsRecS *)r;
        for (int m = 2; m < 8; ++m) if (r->mlen[m]) o += (r->mlen[m] + 15) & ~15ull;
    }
    static int32_t wk[MAX_THREADS]; static uint64_t w1[MAX_THREADS], w2[MAX_THREADS], ws[MAX_THREADS]; int used[MAX_THREADS] = {0};
    for (int j = 0; j < g_nthr; ++j) thread_wait(&g_thr[j], &wk[j], &w1[j], &w2[j], &ws[j]);
    int by_cell = 0, by_site = 0, by_name = 0, by_pool = 0, none = 0, crossed = 0;
    for (int pass = 0; pass < 4; ++pass)
        for (int i = 0; i < nrec; ++i) {
            const HTlsRec *r = rec[i];
            if (r->order >= 1024 || g_l3map[r->order]) continue;
            for (int j = 0; j < g_nthr; ++j) {
                if (used[j] || !same_pool(r->name, g_thr[j].name)) continue;
                int same_name = !strncmp(r->name, g_thr[j].name, 31);
                int ok = pass == 0 ? r->wkind && r->wkind == wk[j] && r->wc1 == w1[j] && r->wc2 == w2[j] && r->site == ws[j] && same_name
                       : pass == 1 ? r->site && r->site == ws[j] && same_name && (uint32_t)g_thr[j].order == r->order
                       : pass == 2 ? !strncmp(r->name, g_thr[j].name, 31)
                       : 1;
                if (!ok) continue;
                used[j] = 1; g_l3map[r->order] = &g_thr[j];
                if (pass == 0) ++by_cell; else if (pass == 1) ++by_site; else if (pass == 2) ++by_name; else ++by_pool;
                if (strncmp(r->name, g_thr[j].name, 31)) ++crossed;
                break;
            }
        }
    for (int i = 0; i < nrec && verbose; ++i) {
        const HTlsRec *r = rec[i]; GThread *g = thr_by_order(r->order);
        int j = g ? (int)(g - g_thr) : -1;
        if (!g) ++none;
        blog("  L3 thread #%u '%s' wait %d 0x%llx/0x%llx site guest+0x%llx -> %s%s #%d '%s' wait %d 0x%llx/0x%llx site guest+0x%llx", r->order, r->name, r->wkind, r->wc1, r->wc2, r->site ? r->site - 0x800000000ull : 0,
             g ? "" : "NONE", g && j >= 0 && r->wkind && r->wkind == wk[j] && r->wc1 == w1[j] ? "[cell] " : "", g ? g->order : -1, g ? g->name : "", j >= 0 ? wk[j] : 0, j >= 0 ? w1[j] : 0, j >= 0 ? w2[j] : 0, j >= 0 && ws[j] ? ws[j] - 0x800000000ull : 0);
    }
    if (verbose) blog("L3 threads: %d matched by wait cell, %d by wait place, %d by name, %d by pool, %d unmatched; %d take another thread's identity", by_cell, by_site, by_name, by_pool, none, crossed);
}

/* Same process: slot thread (creation order at the save) -> the same GuestThread now. */
static void l1_match(const uint8_t *b) {
    const LayoutHdr *h = (const LayoutHdr *)b;
    const LThr *t = (const LThr *)(b + sizeof *h + h->nvma * sizeof(Vma));
    memset(g_l3map, 0, sizeof g_l3map); memset(g_l3rec, 0, sizeof g_l3rec);
    for (uint32_t i = 0; i < h->nthr; ++i)
        for (int j = 0; j < g_nthr; ++j) if (g_thr[j].gt == t[i].gt && t[i].order < 1024) { g_l3map[t[i].order] = &g_thr[j]; break; }
}
/* Guest state on the stacks: a thread here sits at the same wait as the slot's thread (same depth and
 * same guest return addresses) but its frames hold pointers to this run's objects. Every stack slot and
 * callee-saved register where the slot has another guest pointer (or vsync handle) gets the slot's
 * value; pointers into the slot thread's own stack move to this stack; host values stay. */
typedef struct {
    char name[32]; uint32_t tid, order, is_main, pad; uint64_t gt, tcb, teb, stackbase, stacklimit, rsp, tls_addr, tls_len, stack_len, dead_len;
    DECLSPEC_ALIGN(16) CONTEXT ctx;
} TRecL;   /* = TRec */
static uint64_t trec_size(const TRecL *r) { return sizeof *r + ((r->stack_len + 15) & ~15ull) + ((r->tls_len + 15) & ~15ull) + ((r->dead_len + 15) & ~15ull); }
static uint8_t *g_l3tblob; static uint64_t g_l3tlen;
static const TRecL *slot_thread(int32_t order) {
    for (uint64_t off = 0; g_l3tblob && off + sizeof(TRecL) <= g_l3tlen;) {
        const TRecL *r = (const TRecL *)(g_l3tblob + off);
        if ((int32_t)r->order == order) return r;
        off += trec_size(r);
    }
    return NULL;
}
static unsigned long g_tid_a[MAX_THREADS], g_tid_b[MAX_THREADS]; static int g_ntp;
static int tp_skip[MAX_THREADS];  /* g_thr index: guest stack transplanted (not scanned for this run's handles) */
static int guest_ptr(uint64_t v) {
    if (v >= 0x800000000ull && v < 0x805700000ull) return 1;
    for (int i = 0; i < g_nvm; ++i) if (g_vm[i].kind != 1 && v >= g_vm[i].start && v < g_vm[i].end) return 1;
    return 0;
}
/* executable pages of the low area: eboot text and the guest libc modules (vtables etc. are not code) */
static Range g_gcode[64]; static int g_ngcode;
static void find_guest_code(void) {
    g_ngcode = 0;
    for (uint64_t a = 0x800000000ull, end = low_next(); a < end && g_ngcode < 64;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void *)a, &m, sizeof m)) break;
        if (m.State == MEM_COMMIT && (m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) { g_gcode[g_ngcode].lo = a; g_gcode[g_ngcode].hi = a + m.RegionSize; ++g_ngcode; }
        a = (uint64_t)m.BaseAddress + m.RegionSize;
    }
}
static int guest_code(uint64_t v) { for (int i = 0; i < g_ngcode; ++i) if (v >= g_gcode[i].lo && v < g_gcode[i].hi) return 1; return 0; }
typedef struct {
    const TRecL *r; const uint64_t *stack, *dead; GThread *g; int ok; uint64_t n_dead;
    uint64_t rspb, gk, lim, n_val, n_reg; uint64_t *regp[16]; CONTEXT gctx; int match, bad, nseg; uint64_t seg[16][2]; char why[400], ext[200];
} TPlan;
static TPlan g_tp[MAX_THREADS]; static int g_ntpl;
static int host_code(uint64_t v) { for (int j = 0; j < g_nmods; ++j) if (v >= g_mods[j].base + 0x1000 && v < g_mods[j].base + g_mods[j].size) return 1; return 0; }
/* A host return address (not a function pointer kept as data): the bytes before it are a call. */
static int call_before(uint64_t v);
static int host_return(uint64_t v) { return host_code(v) && call_before(v); }
static int guest_return(uint64_t v) { return guest_code(v) && call_before(v); }
static int call_before(uint64_t v) {
    const uint8_t *c = (const uint8_t *)v;
    __try {
        if (c[-5] == 0xE8) return 1;                                              /* call rel32 */
        if (c[-2] == 0xFF && (c[-1] & 0xF8) == 0xD0) return 1;                    /* call reg */
        if (c[-3] == 0xFF && ((c[-2] & 0xF8) == 0x50 || c[-2] == 0x54)) return 1; /* call [reg+disp8] */
        if (c[-4] == 0xFF && c[-3] == 0x54) return 1;                             /* call [sib+disp8] */
        if (c[-6] == 0xFF && (c[-5] == 0x15 || (c[-5] & 0xF8) == 0x90)) return 1; /* call [rip+disp32] / [reg+disp32] */
        if (c[-7] == 0xFF && c[-6] == 0x94) return 1;                             /* call [sib+disp32] */
        if (c[-2] == 0xFF && (c[-1] & 0xF8) == 0x10) return 1;                    /* call [reg] */
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}
/* Unwinds a suspended thread through its host frames (unwind data of ntdll, kernelbase, this DLL,
 * bb-probe) to the first guest frame: c becomes the guest's context at its call into the host
 * (stack pointer above the return address, nonvolatile registers), regp[i] says where the value of
 * register i (CONTEXT order Rax..R15) is saved on the stack (NULL: still in the thread's registers). */
static int unwind_to_guest(const CONTEXT *start, CONTEXT *c, uint64_t *regp[16]) {
    *c = *start;
    KNONVOLATILE_CONTEXT_POINTERS cp; memset(&cp, 0, sizeof cp);
    uint32_t st, self_size; dll_identity(&st, &self_size);
    for (int depth = 0; depth < 64; ++depth) {
        if (guest_code(c->Rip)) { for (int i = 0; i < 16; ++i) regp[i] = (uint64_t *)cp.IntegerContext[i]; return 1; }
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c->Rip, &base, NULL);
        if (!fe) {   /* thunks.asm (sub rsp, 40 before the call) or a leaf */
            if (c->Rip >= (uint64_t)g_self && c->Rip < (uint64_t)g_self + self_size) c->Rsp += 40;
            c->Rip = *(uint64_t *)c->Rsp; c->Rsp += 8;
            continue;
        }
        PVOID hd; DWORD64 est;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c->Rip, fe, c, &hd, &est, &cp);
    }
    return 0;
}
/* The slot's value for this thread here: pointers into its own stack line up at the stack pointer,
 * host objects and other stacks through the XREF ranges. */
static uint64_t a2b(const TPlan *p, uint64_t v) {
    if (v >= p->r->rsp - p->r->dead_len && v < p->r->stackbase) return v - p->r->rsp + p->rspb;
    uint64_t n = 0; xlate_buf(&v, 1, &n);
    return v;
}
/* Host module addresses of the slot (return addresses on its stacks) -> the same place here: module bases
 * move after a PC restart (ASLR), and the DLL copy is loaded elsewhere in every run. Filled before the
 * stacks are compared (the XREF translation table comes later, in l3_apply). */
static struct { uint64_t lo, hi, to; } g_smod[MAX_MODULES]; static int g_nsmod;
static void slot_modules(const uint8_t *b, uint32_t len) {
    uint64_t xl = 0; const XR *xr = (const XR *)layout_ext(b, len, EXT_XREF, &xl);
    g_nsmod = 0;
    for (uint64_t i = 0; xr && i < xl / sizeof(XR) && g_nsmod < MAX_MODULES; ++i) {
        if (xr[i].cat != X_MOD) continue;
        for (int k = 0; k < g_nmods; ++k) {
            int self = is_self_module(xr[i].name) && is_self_module(g_mods[k].name);
            if ((self || !_strnicmp(g_mods[k].name, xr[i].name, sizeof xr[i].name - 1)) && (self || g_mods[k].size == xr[i].hi - xr[i].lo)) {
                g_smod[g_nsmod].lo = xr[i].lo; g_smod[g_nsmod].hi = xr[i].hi; g_smod[g_nsmod].to = g_mods[k].base; ++g_nsmod; break;
            }
        }
    }
}
static uint64_t slot_host(uint64_t v) {
    for (int i = 0; i < g_nsmod; ++i) if (v >= g_smod[i].lo && v < g_smod[i].hi) return v - g_smod[i].lo + g_smod[i].to;
    return v;
}
static const int k_nvreg[] = {3, 5, 6, 7, 12, 13, 14, 15};   /* Rbx Rbp Rsi Rdi R12-R15 */
/* Stacks line up at the stack pointer (the top of each thread's stack sits elsewhere in every run).
 * Guest frames: from the guest's call into the host (unwound, or the frame hook's saved registers for
 * the render thread) up to the next host return address. */
static void tp_plan(void) {
    g_ntpl = 0; g_ntp = 0; memset(tp_skip, 0, sizeof tp_skip);
    find_guest_code();
    for (uint64_t off = 0; g_l3tblob && off + sizeof(TRecL) <= g_l3tlen && g_ntpl < MAX_THREADS;) {
        const TRecL *r = (const TRecL *)(g_l3tblob + off);
        off += sizeof *r;
        TPlan *p = &g_tp[g_ntpl++]; memset(p, 0, sizeof *p);
        p->r = r; p->stack = (const uint64_t *)(g_l3tblob + off);
        off += (r->stack_len + 15) & ~15ull; off += (r->tls_len + 15) & ~15ull;
        p->dead = (const uint64_t *)(g_l3tblob + off); off += (r->dead_len + 15) & ~15ull;
        p->g = thr_by_order(r->order);
        if (!p->g) { strcpy_s(p->why, sizeof p->why, "no thread here"); continue; }
        if (!!r->is_main != !!p->g->is_main) { strcpy_s(p->why, sizeof p->why, "render-thread role differs"); continue; }
        p->rspb = p->g->is_main ? g_pause_frame : p->g->ctx.Rsp;
        if (p->rspb < p->g->stacklimit || p->rspb >= p->g->stackbase) { strcpy_s(p->why, sizeof p->why, "stack pointer outside the thread's stack (on a fiber?)"); continue; }
        const uint64_t *bs = (const uint64_t *)p->rspb;
        uint64_t na = r->stack_len / 8, nb = (p->g->stackbase - p->rspb) / 8, n = na < nb ? na : nb;
        if (p->g->is_main) p->gk = 0;   /* thunks.asm saved the guest's registers at g_pause_frame */
        else {
            if (!unwind_to_guest(&p->g->ctx, &p->gctx, p->regp)) { strcpy_s(p->why, sizeof p->why, "no guest frame within 64 host frames"); continue; }
            p->gk = (p->gctx.Rsp - p->rspb) / 8;
            if (p->gk == 0 || p->gk > n || slot_host(p->stack[p->gk - 1]) != bs[p->gk - 1]) {
                _snprintf_s(p->why, sizeof p->why, _TRUNCATE, "the call into the host differs (guest+0x%llx here, slot +0x%llx has 0x%llx)",
                            p->gctx.Rip - 0x800000000ull, (p->gk - 1) * 8, p->gk && p->gk <= n ? p->stack[p->gk - 1] : 0);
                continue;
            }
            for (int q = 0; q < 8; ++q) {   /* saved-register slots must be inside the host part */
                uint64_t *a = p->regp[k_nvreg[q]];
                if (a && ((uint64_t)a < p->rspb || (uint64_t)a >= p->gctx.Rsp)) { _snprintf_s(p->why, sizeof p->why, _TRUNCATE, "register %d saved outside the host frames", k_nvreg[q]); break; }
            }
            if (p->why[0]) continue;
        }
        /* guest frames: follow the guest's frame-pointer chain (push rbp; mov rbp, rsp) from the call
         * into the host up to the first return address into host code (the thread's start, or a host
         * function that called back into the guest); stale host addresses inside guest frames are data */
        uint64_t rbp = p->g->is_main ? *(uint64_t *)(g_pause_frame + 216) : p->gctx.Rbp, top = n;
        int frames = 0, broken = 0;
        p->nseg = 1;
        for (; frames < 1024; ++frames) {
            if (rbp < p->rspb + p->gk * 8 || rbp + 16 > p->g->stackbase || (rbp & 7)) { broken = 1; break; }
            uint64_t kr = (rbp + 8 - p->rspb) / 8, ret = bs[kr];
            if (kr >= n) { broken = 1; break; }
            if (slot_host(p->stack[kr]) != ret) { ++p->bad; if (p->bad <= 4) { int w = (int)strlen(p->why); _snprintf_s(p->why + w, sizeof p->why - w, _TRUNCATE, " [frame %d +0x%llx slot %llx now %llx]", frames, kr * 8, p->stack[kr], ret); } }
            else ++p->match;
            if (!guest_code(ret)) { top = kr + 1; break; }
            uint64_t next = bs[kr - 1];
            if (next <= rbp) { broken = 1; break; }
            rbp = next;
        }
        if (broken) { _snprintf_s(p->why, sizeof p->why, _TRUNCATE, "frame chain broken after %d guest frames (rbp 0x%llx)", frames, rbp); continue; }
        p->seg[0][0] = p->gk; p->seg[0][1] = top; p->lim = top;
        /* Outer guest frames: a guest function may call the host, which calls back into the guest (the
         * game's main loop runs that way): the frame-pointer chain goes on through host frames (the exe
         * keeps frame pointers) and every further run of guest frames is transplanted as well while the
         * return addresses match the slot. Guest memory points at objects in those frames too (locals of
         * the outermost functions near the top of the main thread's stack): left as they are, the heap
         * of the slot met this run's objects there (crash when the player died after such a load). */
        if (!p->bad && top >= 2 && top < n) {
            /* host frames are unwound with their unwind data (RtlVirtualUnwind: the host code keeps no
             * reliable frame-pointer chain), guest frames follow the frame-pointer chain; every further run
             * of guest frames whose return addresses match the slot becomes a segment */
            DECLSPEC_ALIGN(16) CONTEXT c; memset(&c, 0, sizeof c);
            c.Rip = bs[top - 1]; c.Rsp = p->rspb + top * 8; c.Rbp = bs[top - 2];   /* the guest frame below saved the host's rbp */
            int f; char d[96];
            for (f = 0; f < 256 && p->nseg < 16; ++f) {
                if (c.Rsp <= p->rspb || c.Rsp + 8 > p->g->stackbase || (c.Rsp & 7)) { _snprintf_s(p->ext, sizeof p->ext, _TRUNCATE, "outer walk left the stack after %d steps", f); break; }
                if (guest_code(c.Rip)) {
                    uint64_t start = (c.Rsp - p->rspb) / 8, r2 = c.Rbp, end = 0; int gf, same = 1;
                    for (gf = 0; gf < 1024; ++gf) {
                        if (r2 + 8 < c.Rsp || r2 + 16 > p->g->stackbase || (r2 & 7)) break;
                        uint64_t kr = (r2 + 8 - p->rspb) / 8;
                        if (kr >= n) break;
                        if (slot_host(p->stack[kr]) != bs[kr]) { same = 0; break; }
                        if (!guest_code(bs[kr])) { end = kr + 1; break; }
                        uint64_t nx = bs[kr - 1];
                        if (nx <= r2) break;
                        r2 = nx;
                    }
                    if (!same || !end) { describe(c.Rip, d, sizeof d); _snprintf_s(p->ext, sizeof p->ext, _TRUNCATE, "outer guest frames at %s %s (after %d steps)", d, same ? "end without a host return" : "differ from the slot", f); break; }
                    p->seg[p->nseg][0] = start; p->seg[p->nseg][1] = end; ++p->nseg; p->lim = end;
                    c.Rip = bs[end - 1]; c.Rsp = p->rspb + end * 8; c.Rbp = bs[end - 2];
                    continue;
                }
                DWORD64 base = 0; PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &base, NULL);
                if (!fe) { describe(c.Rip, d, sizeof d); _snprintf_s(p->ext, sizeof p->ext, _TRUNCATE, "no unwind data at %s after %d steps (%d outer guest segments)", d, f, p->nseg - 1); break; }
                PVOID hd; DWORD64 est;
                __try { RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fe, &c, &hd, &est, NULL); }
                __except (EXCEPTION_EXECUTE_HANDLER) { _snprintf_s(p->ext, sizeof p->ext, _TRUNCATE, "unwind fault after %d steps", f); break; }
                if (!c.Rip) { _snprintf_s(p->ext, sizeof p->ext, _TRUNCATE, "reached the thread start after %d steps (%d outer guest segments)", f, p->nseg - 1); break; }
            }
        }
        if (p->bad) {
            char tmp[400]; strcpy_s(tmp, sizeof tmp, p->why);
            _snprintf_s(p->why, sizeof p->why, _TRUNCATE, "%d of %d frames return elsewhere:%s", p->bad, frames + 1, tmp);
            continue;
        }
        p->ok = 1;
        if (g_ntp < MAX_THREADS) { g_tid_a[g_ntp] = r->tid; g_tid_b[g_ntp] = p->g->tid; ++g_ntp; }
        tp_skip[p->g - g_thr] = 1;
    }
}
static void tp_apply(char *note, size_t nn) {
    int done = 0, skipped = 0; uint64_t tot_v = 0, tot_r = 0, tot_d = 0;
    for (int i = 0; i < g_ntpl; ++i) {
        TPlan *p = &g_tp[i];
        if (!p->ok) { ++skipped; blog("  L3 stack '%s' (#%u): not transplanted: %s", p->r->name, p->r->order, p->why); continue; }
        uint64_t *b = (uint64_t *)p->rspb;
        for (int g = 0; g < p->nseg; ++g)
            for (uint64_t k = p->seg[g][0]; k < p->seg[g][1]; ++k) { uint64_t v = a2b(p, p->stack[k]); if (v != b[k]) { b[k] = v; ++p->n_val; } }
        if (!p->g->is_main && p->r->dead_len && g_deadwin) {   /* the dead window: the slot's contents below the stack pointer */
            uint64_t dl = p->r->dead_len, lo = p->rspb - dl;
            if (lo >= p->g->stacklimit && !(lo & 7)) {
                uint64_t *d = (uint64_t *)lo;
                for (uint64_t k = 0; k < dl / 8; ++k) { uint64_t v = a2b(p, p->dead[k]); if (v != d[k]) { d[k] = v; ++p->n_dead; } }
            }
        }
        if (!p->g->is_main)
            for (int q = 0; q < 8; ++q) {
                int ri = k_nvreg[q];
                uint64_t *slot = p->regp[ri];
                uint64_t va = slot ? p->stack[((uint64_t)slot - p->rspb) / 8] : (&p->r->ctx.Rax)[ri];
                uint64_t v = a2b(p, va);
                uint64_t *dst = slot ? slot : &(&p->g->ctx.Rax)[ri];
                if (*dst != v) { *dst = v; ++p->n_reg; }
            }
        if (p->nseg > 1) {
            char seg[1200]; int w = 0; seg[0] = 0;
            uint64_t *bb = (uint64_t *)p->rspb;
            for (int g = 0; g < p->nseg && w < 1100; ++g) {
                char d1[96] = "", d2[96] = "";
                if (p->seg[g][1] < p->lim || g + 1 < p->nseg) describe(bb[p->seg[g][1]], d1, sizeof d1);
                if (g + 1 < p->nseg) describe(bb[p->seg[g + 1][0] - 1], d2, sizeof d2);
                w += _snprintf_s(seg + w, sizeof seg - w, _TRUNCATE, " [0x%llx..0x%llx host %s .. guest %s]", p->seg[g][0] * 8, p->seg[g][1] * 8, d1, d2);
            }
            blog("  L3 segments '%s':%s", p->r->name, seg);
        }
        ++done; tot_v += p->n_val; tot_r += p->n_reg; tot_d += p->n_dead;
        blog("  L3 stack '%s' (#%u -> '%s' tid %lu): rsp 0x%llx (slot 0x%llx), top 0x%llx (slot 0x%llx); %d guest segments 0x%llx..0x%llx: %llu values, %llu registers from the slot (%d guest return addresses same)%s%s",
             p->r->name, p->r->order, p->g->name, p->g->tid, p->rspb, p->r->rsp, p->g->stackbase, p->r->stackbase, p->nseg, p->gk * 8, p->lim * 8, p->n_val, p->n_reg, p->match, p->ext[0] ? "; " : "", p->ext);
    }
    _snprintf_s(note, nn, _TRUNCATE, "stacks: %d threads transplanted (%llu values, %llu registers, %llu values below the stack pointers), %d not", done, tot_v, tot_r, tot_d, skipped);
}
/* At the load's consistent point, guest pages already restored: translate host references from the
 * saving process to this one, put back the thread-local state, continue the process clock. */
static int l3_apply(const uint8_t *b, uint32_t len, char *note, size_t nn) {
    const LayoutHdr *h = (const LayoutHdr *)b;
    const LThr *t = (const LThr *)(b + sizeof *h + h->nvma * sizeof(Vma));
    uint64_t xl = 0, hl = 0, tl = 0;
    const XR *xr = (const XR *)layout_ext(b, len, EXT_XREF, &xl);
    const HR *hr = (const HR *)layout_ext(b, len, EXT_HREG, &hl);
    const uint8_t *ht = layout_ext(b, len, EXT_HTLS, &tl);
    const L3Info *in = (const L3Info *)layout_ext(b, len, EXT_INFO, NULL);

    g_nxr = (int)(xl / sizeof(XR)); if (g_nxr > XR_MAX) g_nxr = XR_MAX;
    memcpy(g_xr, xr, g_nxr * sizeof(XR));
    g_nhr = (int)(hl / sizeof(HR)); if (g_nhr > HR_MAX) g_nhr = HR_MAX;
    if (hr) memcpy(g_hr, hr, g_nhr * sizeof(HR));
    /* the same objects here */
    uint8_t *port = vo_port();
    int32_t eqk[16]; uint8_t *eqs[16]; int neq = vo_equeues(eqk, eqs, 16);
    int unmapped = 0, mods = 0, newblk = 0, ident[X_NCAT] = {0}; char um[300]; int un = 0; um[0] = 0;
    for (int i = 0; i < g_nxr; ++i) {
        XR *r = &g_xr[i]; r->to = 0;
        GThread *g = r->cat <= X_MTLS || r->cat == X_STACK ? thr_by_order(r->cat == X_MTLS ? (uint32_t)r->idx / 8 : (uint32_t)r->idx) : NULL;
        switch (r->cat) {
        case X_GT: if (g) r->to = g->gt; break;
        case X_TCB: if (g) r->to = g->tcb; break;
        case X_TLS: if (g && g->tcb - g->tls_addr == r->hi - r->lo) r->to = g->tls_addr; break;
        case X_MTLS: if (g) {
            uint64_t *blocks = (uint64_t *)tls_var(g->teb, RVA_TLSV_TLS_BLOCKS);
            int m = r->idx % 8;
            if (blocks && !blocks[m]) { blocks[m] = (uint64_t)VirtualAlloc(NULL, r->hi - r->lo, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); ++newblk; }
            if (blocks) r->to = blocks[m];
        } break;
        case X_VO: r->to = (uint64_t)port; break;
        case X_STACK: {   /* stacks line up at the stack pointer (the top sits elsewhere in each run) */
            const TRecL *tr = slot_thread(r->idx);
            if (g && tr) { uint64_t rspb = g->is_main ? g_pause_frame : g->ctx.Rsp; r->to = r->lo + rspb - tr->rsp; }
        } break;
        case X_EQ: for (int k = 0; k < neq; ++k) if (eqk[k] == r->idx) r->to = (uint64_t)eqs[k]; break;
        case X_MOD:
            for (int k = 0; k < g_nmods; ++k) {
                /* this DLL (another build may differ in size): only stale return addresses of finished calls
                 * point into it, the guest calls it through the gate */
                int self = is_self_module(r->name) && is_self_module(g_mods[k].name);
                if ((self || !_strnicmp(g_mods[k].name, r->name, sizeof r->name - 1)) && (self || g_mods[k].size == r->hi - r->lo)) { r->to = g_mods[k].base; ++mods; break; }
            }
            break;
        }
        if (r->to && r->to == r->lo) { r->to = 0; ++ident[r->cat]; continue; }   /* the same address here (fixed stacks): nothing to rewrite */
        if (!r->to) { ++unmapped; if (un < 280) un += _snprintf_s(um + un, sizeof um - un, _TRUNCATE, " %s:%s", k_xname[r->cat], r->name); }
    }
    double ms;
    if (g_xstats) { ms = xscan(1, &g_xs); xstats_log("LOAD L3 translate", &g_xs, ms); }
    else {   /* fast path: rewrite only; hits per range are not counted */
        memset(&g_xs, 0, sizeof g_xs);
        for (int i = 0; i < g_nxr; ++i) g_xr[i].hits = 0;
        ms = xscan_parallel(&g_xs.translated);
        blog("LOAD L3 translate: %llu values rewritten in %.0f ms (%d threads)", g_xs.translated, ms, store_threads());
    }
    if (g_xstats) { uint32_t st, sz; dll_identity(&st, &sz); uint64_t self_hits = 0, gate_hits = 0;
      for (int i = 0; i < g_nxr; ++i) { if (g_xr[i].cat != X_MOD) continue; if (is_self_module(g_xr[i].name)) self_hits += g_xr[i].hits; else if (!strcmp(g_xr[i].name, "bbgate")) gate_hits += g_xr[i].hits; }
      blog("LOAD L3: values into the gate %llu, into bbstate.dll itself %llu; DLL build of the slot %08x/%u, now %08x/%u%s", gate_hits, self_hits,
           in ? in->dll_stamp : 0, in ? in->dll_size : 0, st, sz, in && (in->dll_stamp != st || in->dll_size != sz) ? " (another build)" : ""); }
    /* static TLS data (the TCB stays this thread's), host thread-locals, module TLS contents */
    uint64_t off = sizeof *h + h->nvma * sizeof(Vma) + h->nthr * sizeof(LThr) + sizeof(VOState) + sizeof(FState) + sizeof(SemState), xt = 0;
    int tls_done = 0, keys_done = 0;
    for (uint32_t i = 0; i < h->nthr; ++i) {
        GThread *g = thr_by_order(t[i].order);
        uint64_t data = t[i].tcb - t[i].tls_addr;
        if (g && off + t[i].tls_len <= len && g->tcb - g->tls_addr == data) {
            memcpy((void *)g->tls_addr, b + off, data); xlate_buf((uint64_t *)g->tls_addr, data / 8, &xt); ++tls_done;
        }
        off += t[i].tls_len;
    }
    for (uint64_t o = 0; ht && o + sizeof(HTlsRec) <= tl;) {
        const HTlsRec *r = (const HTlsRec *)(ht + o);
        o += sizeof *r;
        GThread *g = thr_by_order(r->order);
        uint64_t *keys = g ? (uint64_t *)tls_var(g->teb, RVA_TLSV_KEY_VALUES) : NULL, *blocks = g ? (uint64_t *)tls_var(g->teb, RVA_TLSV_TLS_BLOCKS) : NULL;
        int32_t *err = g ? (int32_t *)tls_var(g->teb, RVA_TLSV_GUEST_ERRNO) : NULL;
        if (keys) { memcpy(keys, r->keys, sizeof r->keys); xlate_buf(keys, 256, &xt); ++keys_done; }
        if (err) *err = r->err;
        for (int m = 2; m < 8; ++m) {
            if (!r->mlen[m]) continue;
            if (blocks && blocks[m]) { memcpy((void *)blocks[m], ht + o, r->mlen[m]); xlate_buf((uint64_t *)blocks[m], r->mlen[m] / 8, &xt); }
            o += (r->mlen[m] + 15) & ~15ull;
        }
    }
    /* process clock: never backwards for the restored guest */
    uint64_t now = rt_ptime(); int64_t shift = 0;
    if (in && in->ptime_us > now) {
        shift = (int64_t)(in->ptime_us - now) + 1000;
        int64_t *ps = (int64_t *)(g_base + RVA_PROCESS_START);   /* struct timespec { int64 sec; long nsec; } */
        int64_t ns = ps[0] * 1000000000ll + *(int32_t *)(ps + 1) - shift * 1000;
        ps[0] = ns / 1000000000ll; *(int32_t *)(ps + 1) = (int32_t)(ns % 1000000000ll);
    }
    /* TSC and monotonic clock: forward to the slot's values if they are ahead (a PC restart since the save) */
    int64_t tsc_fwd = 0, mono_fwd = 0;
    if (in && g_clockfix) {
        uint64_t tn = vc_tsc_c();
        if (in->tsc_guest > tn) { tsc_fwd = (int64_t)(in->tsc_guest - tn) + (int64_t)(g_tsc_hz / 1000); g_tsc_off += tsc_fwd; }
        int64_t mn = mono_guest_ns();
        if (in->mono_guest_ns > mn) { mono_fwd = in->mono_guest_ns - mn + 1000000; g_mono_off_ns += mono_fwd; }
    }
    blog("LOAD L3: ranges at the same address here (not translated): %d stacks, %d GuestThread, %d TCB, %d static TLS, %d module TLS, %d modules",
         ident[X_STACK], ident[X_GT], ident[X_TCB], ident[X_TLS], ident[X_MTLS], ident[X_MOD]);
    _snprintf_s(note, nn, _TRUNCATE, "L3: %d host ranges (%d modules mapped, %d unmapped%s), %llu values rewritten in guest memory + %llu in TLS; static TLS %d threads, host thread-locals %d, %d module TLS blocks created; process clock %s %lld ms (slot %llu ms, now %llu ms); TSC %s %.1f s, monotonic clock %s %.1f s",
                g_nxr, mods, unmapped, um, g_xs.translated, xt, tls_done, keys_done, newblk, shift ? "moved forward" : "kept,", shift / 1000, in ? in->ptime_us / 1000 : 0, now / 1000,
                tsc_fwd ? "moved forward" : "kept", g_tsc_hz ? (double)tsc_fwd / g_tsc_hz : 0.0, mono_fwd ? "moved forward" : "kept", mono_fwd / 1e9);
    return 1;
}

/* ---- operations at the consistent point ---- */
static Range *g_dirty; static uint64_t g_ndirty, g_dirty_cap = 1u << 20;
static void on_dirty(uint64_t a, uint64_t len) {
    if (g_ndirty && g_dirty[g_ndirty - 1].hi == a) { g_dirty[g_ndirty - 1].hi = a + len; return; }
    if (g_ndirty == g_dirty_cap) { g_dirty[g_ndirty - 1].hi = a + len; return; }
    g_dirty[g_ndirty].lo = a; g_dirty[g_ndirty].hi = a + len; ++g_ndirty;
}
/* GPU cache invalidation (Rasterizer::InvalidateMemory). For a range the GPU wrote, the port first
 * downloads the GPU data into guest memory (readbacks): so it runs before the slot's pages are
 * written, and again on the written ranges after the threads resume. */
static void gpu_invalidate(uint64_t a, uint64_t len) {
    void (*inv)(uint64_t, uint64_t) = *(void (**)(uint64_t, uint64_t))(g_base + RVA_HOOK_INVALIDATE);
    if (inv) inv(a, len);
}
/* Port readbacks (Relaxed): data the GPU wrote stays on the GPU until a CPU write to that page
 * flushes it. A snapshot of guest memory alone would miss it (and a load would leave the current
 * timeline's GPU data to be flushed over the restored pages later). Rasterizer::ReadMemory
 * downloads every GPU-modified range of the given span into guest memory without marking it
 * CPU-modified; called in 256 MB pieces (buffer cache arenas are 1-4 GB). */
static double gpu_flush(void) {
    double t0 = now_ms();
    uint8_t *mm = *(uint8_t **)(g_base + RVA_MEMMGR_INSTANCE);
    void *rast = mm ? *(void **)(mm + 8) : NULL;
    if (!rast) return 0;
    char (*rd)(void *, uint64_t, uint64_t, char) = (char (*)(void *, uint64_t, uint64_t, char))(g_base + RVA_RAST_READMEMORY);
    static Vma v[MAX_VMAS];
    int n = read_vmas(v, MAX_VMAS);
    for (int i = 0; i < n; ++i) {
        if (v[i].kind == 1) continue;
        for (uint64_t a = v[i].start; a < v[i].end; ) {
            uint64_t e = (a & ~((256ull << 20) - 1)) + (256ull << 20);
            if (e > v[i].end) e = v[i].end;
            rd(rast, a, e - a, 0);
            a = e;
        }
    }
    return now_ms() - t0;
}
static void mem_note(const char *tag) {
    PROCESS_MEMORY_COUNTERS_EX pm; pm.cb = sizeof pm;
    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm);
    DWORD handles = 0; GetProcessHandleCount(GetCurrentProcess(), &handles);
    blog("%s: process private %llu MB, working set %llu MB, handles %lu; store RAM %llu MB (peak %llu MB); objects kept %ld, tracked %ld",
         tag, (uint64_t)pm.PrivateUsage >> 20, (uint64_t)pm.WorkingSetSize >> 20, handles, store_ram_bytes() >> 20, store_ram_peak() >> 20, g_nkept, g_tab_count);
}
static int g_busy;
/* Pauses the game at a consistent point; on success the render thread is stopped and all other
 * guest threads are suspended (caller must resume_all + release_main). */
static int reach_point(const char *nm, double *t_ready) {
    for (int attempt = 1; attempt <= 60; ++attempt) {
        double t0 = now_ms(), gpu_ms = 0;
        if (!pause_main(3000)) { blog("%s failed: render thread did not reach %s in 3 s (loading screen or game stalled?)", nm, k_names[g_pause_kind]); return 0; }
        if (!wait_gpu_idle(1000, &gpu_ms)) { release_main(); Sleep(5); continue; }
        double tf = now_ms();
        while (!vo_flips_idle() && now_ms() - tf < 150) Sleep(0);
        if (!vo_flips_idle()) { blog("%s attempt %d: flips still in flight after 150 ms, next frame", nm, attempt); release_main(); Sleep(5); continue; }
        gpu_ms += now_ms() - tf;
        if (!enumerate_threads()) { release_main(); blog("%s failed: thread enumeration", nm); return 0; }
        int quiet = quiesce(attempt > 3 ? 0 : -1);
        if (!quiet) { resume_all(); release_main(); Sleep(1); continue; }
        *t_ready = now_ms() - t0;
        HostQ q1 = host_q(); char hq[400]; host_q_str(&q1, hq, sizeof hq);
        blog("%s attempt %d: consistent point (%d guest threads, GPU idle after %.1f ms); host: %s", nm, attempt, g_nthr, gpu_ms, hq);
        g_hostq_at_point = q1;
        return 1;
    }
    blog("%s failed: no consistent point in 60 frames", nm);
    return 0;
}
static int g_cur_op, g_cur_slot;   /* the save/load reported to api.c (bbs_api.h status and events) */
static void refuse(const char *what, const char *reason) { blog("%s REFUSED: %s", what, reason); play(SND_REFUSED); api_end(g_cur_op, g_cur_slot, 0, reason, 0); }
/* Conditions under which saving or loading is unsafe; fills reason. */
static int unsafe(int for_load, char *reason, size_t n) {
    Player p = read_player();
    if (!p.ok) { _snprintf_s(reason, n, _TRUNCATE, "no player (title, loading screen or area transition)"); return 1; }
    /* No reliable loading-screen flag yet: 0x8053FACDC looked like one (0 on the travel loading
     * screen) but also stays 0 for up to 2.3 s in normal play. The start of a lamp travel (fade-out,
     * player still present) is therefore not refused: loading there cancels the travel. */
    if (!for_load && p.hp <= 0) { _snprintf_s(reason, n, _TRUNCATE, "player is dead"); return 1; }
    return 0;
}
static uint8_t g_thumb[META_THUMB_BYTES];
static float g_appear_s;   /* seconds since the player appeared, at the save (EXT_APPR) */
static double g_player_appear_t;   /* now_ms() when the watcher saw a new PlayerIns */
static uint64_t g_load_ins;        /* the PlayerIns a load brought back: not an appearance */
/* the game's save files (copied into a slot as a fallback): save_files= in the ini, else the port's own save
 * folder <user_root>/savedata/1/CUSA00207/SPRJ0005 (user_root is set by the port at start, wherever the launcher keeps saves) */
static const char *save_files_dir(void) {
    static char dir[MAX_PATH];
    if (O.save_files[0]) return O.save_files;
    dir[0] = 0;
    __try {
        const char *root = (const char *)(g_base + RVA_USER_ROOT);
        if (root[0] && strnlen(root, 512) < 512) _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s/savedata/1/CUSA00207/SPRJ0005", root);
    } __except (EXCEPTION_EXECUTE_HANDLER) { dir[0] = 0; }
    if (dir[0] && GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES) dir[0] = 0;
    return dir;
}
static void do_save(int slot) {
    char reason[256];
    g_cur_op = BBS_OP_SAVE; g_cur_slot = slot; api_begin(BBS_OP_SAVE, slot);
    if (unsafe(0, reason, sizeof reason)) { refuse("SAVE", reason); return; }
    uint64_t free_b = store_free_bytes();
    if (free_b < (uint64_t)O.min_free_mb << 20) {
        _snprintf_s(reason, sizeof reason, _TRUNCATE, "not enough disk space for the slots: %llu MB free, %d MB needed (bbstate.ini min_free_mb)", free_b >> 20, O.min_free_mb);
        refuse("SAVE", reason); return;
    }
    double tm0 = now_ms();
    SlotMeta sm; int have_meta = meta_read_game(&sm), have_thumb = ov_thumbnail(g_thumb, 200) || meta_capture(g_thumb);   /* before the pause: the next presented frame (swapchain copy), else the screen */
    g_appear_s = g_player_appear_t ? (float)((now_ms() - g_player_appear_t) / 1000) : 1e9f;
    sm.same_run_only = !g_early_ok && g_appear_s < EARLY_SAVE_S;
    double meta_ms = now_ms() - tm0;
    double t_ready, t0 = now_ms();
    Player pl = read_player();
    if (g_virt) refresh_modules();
    if (!reach_point("SAVE", &t_ready)) { refuse("SAVE", "no consistent point"); return; }
    double t1 = now_ms();
    InterlockedIncrement(&g_gen); /* objects made from here on are not in this snapshot */
    if (!collect_regions()) { resume_all(); release_main(); refuse("SAVE", "too many memory regions"); return; }
    double flush_ms = gpu_flush();
    SaveStats st;
    int ok = store_save_pages(g_reg, g_nreg, &st);
    if (ok && st.staged_bytes * 6 / 10 + (256ull << 20) > free_b) {   /* zstd keeps ~half of a page: the new pages would not fit */
        store_save_abort(); resume_all(); release_main();
        _snprintf_s(reason, sizeof reason, _TRUNCATE, "not enough disk space: %llu MB of new pages, %llu MB free", st.staged_bytes >> 20, free_b >> 20);
        refuse("SAVE", reason); return;
    }
    g_next = 0;
    uint64_t vslen = 0;
    static uint32_t asc[ASC_RINGS];
    if (ok) { capture_threads(); capture_objects(); vo_capture(&g_vo); files_capture(&g_files); sema_capture(&g_sema); tls_capture();
              memcpy(asc, g_base + RVA_ASC_NEXT_OFFS, sizeof asc); ext_add(EXT_ASCQ, asc, sizeof asc);
              if (g_ajm) { ajm_capture(); if (!g_ajm_trunc) ext_add(EXT_AJM, g_ajm, g_ajm_len); }
              ext_add(EXT_APPR, &g_appear_s, sizeof g_appear_s); }
    if (ok && g_virt) { vslen = vs_capture(g_vsbuf, VS_CAPTURE_MAX); if (!vslen) ok = 0; else ext_add(EXT_VSYNC, g_vsbuf, vslen); }
    if (ok && g_virt) capture_l3();
    double t2 = now_ms();
    HostQ qe = host_q();
    resume_all(); release_main();
    double pause = t2 - t0;
    { char a[400]; host_q_str(&qe, a, sizeof a); blog("SAVE slot %d: host at the end of the pause: %s%s", slot, a,
          memcmp(&qe, &g_hostq_at_point, sizeof qe) ? " (CHANGED during the pause)" : " (unchanged)"); }
    if (!ok) { refuse("SAVE", "store failed (see above)"); return; }
    { int nz = 0; for (uint32_t i = 0; i < g_sema.n; ++i) nz += g_sema.s[i].count != 0;
      blog("SAVE slot %d: %u open files, %u semaphores (%d non-zero)", slot, g_files.n, g_sema.n, nz); }
    blog("SAVE slot %d: VideoOut flip count %llu arg %lld buffer %d, %u equeue events (%u triggered)", slot, g_vo.flip.count, g_vo.flip.flip_arg, g_vo.flip.current_buffer, g_vo.nev, g_vo.nev ? g_vo.ev[0].trig : 0);
    blog("SAVE slot %d: GPU readback flush %.0f ms%s", slot, flush_ms, g_virt ? "" : " (no handle virtualization)");
    if (g_virt) blog("SAVE slot %d: vsync table %llu KB (high water %u; live mutex %u cond %u attr %u rwlock %u sema %u)", slot, vslen >> 10, vs_high_water(), vs_live(VS_MUTEX), vs_live(VS_COND), vs_live(VS_MATTR), vs_live(VS_RWLOCK), vs_live(VS_SEMA));
    blog("SAVE slot %d: game paused %.0f ms (reach %.0f, hash+lookup %.0f, copy %.0f): %llu pages, zero %llu, already in pool %llu, new %llu (%llu MB staged); image %llu KB; %d threads",
         slot, pause, t_ready, st.hash_ms, st.copy_ms, st.pages, st.zero, st.known, st.staged, st.staged_bytes >> 20, g_img_bytes >> 10, g_nthr);
    uint32_t llen; uint8_t *lay = build_layout(&llen);
    char *meta = build_meta(slot, &pl, &st, pause);
    ok = store_save_commit(slot, meta, lay, llen, g_tblob, (uint32_t)g_tblob_len, g_oblob, (uint32_t)g_oblob_len, save_files_dir());
    free(lay); free(meta);
    if (g_virt) { vs_slot_commit(slot, ok); uint32_t kept; uint32_t fr = vs_hygiene(&kept); blog("SAVE slot %d: quarantine: %u destroyed objects freed (no slot of this run needs them), %u kept", slot, fr, kept); }
    if (ok) {
        char dir[MAX_PATH]; core_slot_dir(slot, dir, sizeof dir);
        if (have_meta) meta_write(dir, &sm, "", have_thumb ? g_thumb : NULL);
        blog("SAVE slot %d: %s (%s), play time %llu s, level %d, echoes %u, insight %d, hp %d/%d; thumbnail %s; %.0f ms before the pause; %.1f s after the player appeared%s",
             slot, have_meta ? sm.location : "no game data", have_meta ? sm.map : "", have_meta ? sm.play_time_ms / 1000 : 0, sm.level, sm.echoes, sm.insight, sm.hp, sm.hp_max,
             have_thumb ? "taken" : "none (game window not in front or black)", meta_ms, g_appear_s, sm.same_run_only ? " (area still loading: this slot loads only in this run)" : "");
    }
    blog("SAVE slot %d %s, total %.0f ms (game ran again after %.0f ms)", slot, ok ? "done" : "FAILED", now_ms() - t0, pause);
    (void)t1;
    mem_note("after save");
    play(ok ? SND_SAVED : SND_REFUSED);
    api_end(BBS_OP_SAVE, slot, ok, ok ? "" : "the slot could not be written (see bbstate.log)", pause);
}
static int do_load(int slot, const char *why) {
    char reason[300];
    g_cur_op = BBS_OP_LOAD; g_cur_slot = slot; api_begin(BBS_OP_LOAD, slot);
    int ex = store_slot_exists(slot);
    if (!ex) { _snprintf_s(reason, sizeof reason, _TRUNCATE, "slot %d is empty", slot); refuse("LOAD", reason); return 0; }
    if (unsafe(1, reason, sizeof reason)) { refuse("LOAD", reason); return 0; }
    if (store_slot_damaged(slot)) { _snprintf_s(reason, sizeof reason, _TRUNCATE, "slot %d is damaged: %d pages cannot be read from the pool (see the pool check at start)", slot, store_slot_damaged(slot)); refuse("LOAD", reason); return 0; }
    double t0 = now_ms();
    void *lay; uint32_t llen;
    if (!store_load_prepare(slot, &lay, &llen)) { refuse("LOAD", "cannot read the slot"); return 0; }
    if (!layout_matches((uint8_t *)lay, llen, 0, reason, sizeof reason) || !files_restore(layout_files((uint8_t *)lay, llen), 0, reason, sizeof reason)) {
        free(lay); store_load_done(); refuse("LOAD", reason); return 0;
    }
    int l1tp = !g_l3 && g_virt && g_l1_stacks;   /* same process: guest frames of the stacks from the slot too */
    if (g_l3 || l1tp) { free(g_l3tblob); g_l3tblob = (uint8_t *)store_read_threads(slot, &g_l3tlen); if (!g_l3tblob) l1tp = 0; }
    g_nsmod = 0;
    if (g_l3) {
        store_load_prefetch();
        refresh_modules();
        slot_modules((uint8_t *)lay, llen);
        blog("LOAD slot %d: %s: L3 load (thread stacks %llu KB)%s%s", slot,
             g_l3_same ? "same game process, but the location was reloaded since the save" : "saved by another game process", g_l3tlen >> 10,
             g_l3_other_loc ? "; other location: " : "", g_l3_other_loc ? g_l3_loc_note : "");
    }
    ajm_prepare((uint8_t *)lay, llen);
    double t_prep = now_ms() - t0, t_ready;
    if (!reach_point("LOAD", &t_ready)) { free(lay); store_load_done(); refuse("LOAD", "no consistent point"); return 0; }
    if (g_l3 || l1tp) {   /* a frame where every thread waits in the same place (same guest call chain) as in the slot */
        uint64_t tl = 0; const uint8_t *ht = layout_ext((uint8_t *)lay, llen, EXT_HTLS, &tl);
        int tries = 0, bad = 0, best = 1 << 30, max_tries = g_l3 ? O.l3_tries : O.l1_tries;
        for (;; ++tries) {
            if (g_l3) l3_match(ht, tl, tries == 0); else l1_match((uint8_t *)lay);
            tp_plan();
            bad = 0; for (int i = 0; i < g_ntpl; ++i) bad += !g_tp[i].ok;
            if (bad < best) best = bad;
            if (!bad || tries >= max_tries) break;
            resume_all(); release_main(); Sleep(1 + tries % 7);
            if (!reach_point("LOAD", &t_ready)) { free(lay); store_load_done(); refuse("LOAD", "no consistent point"); return 0; }
        }
        blog("LOAD slot %d: %d threads differ from the slot after %d tries%s", slot, bad, tries + 1, bad ? " (loading anyway)" : "");
        for (int i = 0, n = 0; i < g_ntpl && n < 12; ++i) if (!g_tp[i].ok) { ++n; blog("  differs: '%s' (#%u): %s", g_tp[i].r->name, g_tp[i].r->order, g_tp[i].why); }
    }
    double t1 = now_ms();
    if (!layout_matches((uint8_t *)lay, llen, 1, reason, sizeof reason) || !files_restore(layout_files((uint8_t *)lay, llen), 0, reason, sizeof reason) || !collect_regions()) {
        resume_all(); release_main(); free(lay); store_load_done(); refuse("LOAD", reason); return 0;
    }
    g_ndirty = 0;
    double flush_ms = gpu_flush();
    LoadStats st;
    if (g_l3) {   /* another process: the GPU caches hold that process's view of every address, drop them all */
        double ti = now_ms();
        for (int r = 0; r < g_nreg; ++r) gpu_invalidate(g_reg[r].addr, g_reg[r].size);
        blog("LOAD slot %d: GPU caches invalidated for all guest memory before the write (%.0f ms)", slot, now_ms() - ti);
    }
    int ok = store_load_pages(g_reg, g_nreg, gpu_invalidate, on_dirty, &st);
    if (g_l3) { g_ndirty = 0; for (int r = 0; r < g_nreg; ++r) on_dirty(g_reg[r].addr, g_reg[r].size); }
    if (g_remap_active) { double rms; uint64_t nr = remap_memory(&rms); blog("LOAD slot %d: REMAP %d threads, %llu GuestThread* values rewritten in %.0f ms", slot, g_nremap, nr, rms); }
    char vonote[256];
    vo_restore(layout_vo((uint8_t *)lay, llen), vonote, sizeof vonote);
    /* GPU compute queues: the guest's ring positions came back with memory, the port's own position of
     * every ring must too (else the next sceGnmDingDong submits stale ring contents: "Invalid PM4") */
    int asc_moved = -1;
    { uint64_t al = 0; const uint32_t *asc = (const uint32_t *)layout_ext((uint8_t *)lay, llen, EXT_ASCQ, &al);
      if (asc && al == ASC_RINGS * 4) { uint32_t *cur = (uint32_t *)(g_base + RVA_ASC_NEXT_OFFS); asc_moved = 0;
          for (int i = 0; i < ASC_RINGS; ++i) if (cur[i] != asc[i]) { cur[i] = asc[i]; ++asc_moved; } } }
    char fnote[300];
    if (!files_restore(layout_files((uint8_t *)lay, llen), 1, fnote, sizeof fnote)) ok = 0;
    char snote[400], tnote[160], vnote[700] = "", lnote[700] = "", tpnote[200] = "";
    sema_restore(layout_sema((uint8_t *)lay, llen), snote, sizeof snote);
    char anote[200]; ajm_restore((uint8_t *)lay, llen, anote, sizeof anote);
    if (g_l3) { l3_apply((uint8_t *)lay, llen, lnote, sizeof lnote); _snprintf_s(tnote, sizeof tnote, _TRUNCATE, "TLS: see L3"); }
    else tls_restore((uint8_t *)lay, llen, tnote, sizeof tnote);
    if (g_virt) {
        uint64_t vl = 0; const uint8_t *vs = layout_ext((uint8_t *)lay, llen, EXT_VSYNC, &vl);
        /* thread stacks and saved registers are not restored: handles there name objects of now */
        static VsRange ext[2 * MAX_THREADS]; static CONTEXT before[MAX_THREADS]; int ne = 0;
        if (g_l3) {
            for (int i = 0; i < g_ntpl; ++i) {
                const HTlsRec *r = g_tp[i].ok && g_tp[i].r->order < 1024 ? (const HTlsRec *)g_l3rec[g_tp[i].r->order] : NULL;
                if (r && r->wkind) vs_set_thread_wait(g_tp[i].g->tid, r->wc1, r->wc2);
            }
        }
        for (int i = 0; i < g_nthr; ++i) {
            uint64_t rsp = (g_thr[i].is_main ? g_pause_frame : g_thr[i].ctx.Rsp) & ~7ull;
            if (rsp && rsp < g_thr[i].stackbase) { ext[ne].p = (uint64_t *)rsp; ext[ne].n = (g_thr[i].stackbase - rsp) / 8; ++ne; }
            if (!g_thr[i].is_main) { before[i] = g_thr[i].ctx; ext[ne].p = (uint64_t *)&g_thr[i].ctx.Rax; ext[ne].n = 16; ++ne; }
        }
        if (!vs_restore(vs, vl, ext, ne, !g_l3, g_l3 ? g_tid_a : NULL, g_l3 ? g_tid_b : NULL, g_l3 ? g_ntp : 0, vnote, sizeof vnote)) ok = 0;
        if (l1tp && g_l1_stacks == 2) g_nxr = 0;   /* same process: no host references to translate */
        if (g_l3 || (l1tp && g_l1_stacks == 2)) tp_apply(tpnote, sizeof tpnote);
        for (int i = 0; i < g_nthr; ++i)
            if (!g_thr[i].is_main && memcmp(&before[i].Rax, &g_thr[i].ctx.Rax, 16 * 8)) {
                CONTEXT c = g_thr[i].ctx; c.ContextFlags = CONTEXT_INTEGER;
                blog("LOAD: vsync handle in the registers of thread '%s' moved: %s", g_thr[i].name, SetThreadContext(g_thr[i].h, &c) ? "set" : "SetThreadContext FAILED");
            }
    }
    { HostQ qe = host_q(); char a[400]; host_q_str(&qe, a, sizeof a); blog("LOAD slot %d: host before the threads resume: %s%s", slot, a,
          memcmp(&qe, &g_hostq_at_point, sizeof qe) ? " (CHANGED during the pause)" : " (unchanged)"); }
    uint64_t vfirst0[8] = {0}, vd_paused = g_verify ? store_load_verify(g_reg, g_nreg, vfirst0, 8) : 0;   /* all guest threads still stopped */
    resume_all();
    vo_wake();
    /* GPU caches: after the threads run again (Rasterizer may take the heap lock); the render thread
     * is still stopped, so no new graphics work is submitted meanwhile. */
    void (*inv)(uint64_t, uint64_t) = *(void (**)(uint64_t, uint64_t))(g_base + RVA_HOOK_INVALIDATE);
    uint64_t vfirst[8] = {0}, vd0 = vd_paused, vd1 = 0, vfirst2[8] = {0}, vd_run = g_verify ? store_load_verify(g_reg, g_nreg, vfirst2, 8) : 0;
    double t2 = now_ms();
    if (inv && !g_noinv) for (uint64_t i = 0; i < g_ndirty; ++i) inv(g_dirty[i].lo, g_dirty[i].hi - g_dirty[i].lo);
    double t3 = now_ms();
    if (g_verify) {
        vd1 = store_load_verify(g_reg, g_nreg, vfirst, 8);
        blog("VERIFY: pages that differ from the slot: %llu before the threads resume (first 0x%llx 0x%llx), %llu after they ran (render thread still stopped; first 0x%llx 0x%llx 0x%llx), %llu after the GPU invalidation (first: 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx)",
             vd0, vfirst0[0], vfirst0[1], vd_run, vfirst2[0], vfirst2[1], vfirst2[2], vd1, vfirst[0], vfirst[1], vfirst[2], vfirst[3], vfirst[4], vfirst[5]);
    }
    release_main();
    ajm_finish();
    free(lay); store_load_done();
    blog("LOAD slot %d (%s) %s: game paused %.0f ms (reach %.0f, compare %.0f, GPU pre-invalidate %.0f, write %.0f, invalidate %.0f), prepare %.0f ms: %llu pages changed (%llu zeroed, %llu from RAM staging, %llu from disk = %llu KB read, %llu were overwritten by GPU readback before restore) in %llu ranges",
         slot, why, ok ? "done" : "WITH ERRORS", t3 - t1 + t_ready, t_ready, st.compare_ms, st.before_ms, st.ms - st.compare_ms - st.before_ms, t3 - t2, t_prep, st.changed, st.zeroed, st.from_staging, st.from_disk, st.disk_bytes >> 10, st.gpu_overwritten, g_ndirty);
    { double pm; uint64_t pb; store_load_prepare_stats(&pm, &pb); blog("LOAD slot %d: packs mapped in prepare (%.0f ms, prefetch of %llu MB asked)", slot, pm, pb >> 20); }
    blog("LOAD slot %d: GPU readback flush %.0f ms; VideoOut %s; compute queue positions %s %d; files: %s", slot, flush_ms, vonote, asc_moved < 0 ? "NOT IN THE SLOT" : "restored, changed:", asc_moved, fnote);
    blog("LOAD slot %d: %s; %s; %s", slot, snote, tnote, anote);
    if (vnote[0]) blog("LOAD slot %d: %s", slot, vnote);
    if (lnote[0]) blog("LOAD slot %d: %s", slot, lnote);
    if (tpnote[0]) blog("LOAD slot %d: %s", slot, tpnote);
    if (g_virt) { uint32_t kept; uint32_t fr = vs_hygiene(&kept); if (fr) blog("LOAD slot %d: quarantine: %u destroyed objects freed, %u kept", slot, fr, kept); }
    { SYSTEMTIME tm; GetLocalTime(&tm); _snprintf_s(g_last_load_note, sizeof g_last_load_note, _TRUNCATE, "slot %d at %02d:%02d:%02d (%s)", slot, tm.wHour, tm.wMinute, tm.wSecond, g_l3 ? (g_l3_same ? "L3 same process" : "L3 other process") : "L1"); }
    mem_note("after load");
    play(ok ? SND_LOADED : SND_REFUSED);
    g_load_ins = read_player().ins;
    api_end(BBS_OP_LOAD, slot, ok, ok ? "" : "the load finished with errors (see bbstate.log)", t3 - t1 + t_ready);
    return ok;
}
static void threads_report(void) {
    double t;
    if (!reach_point("THREADS", &t)) return;
    blog("THREADS: %d guest threads", g_nthr);
    for (int i = 0; i < g_nthr; ++i) {
        char rip[160]; uint64_t rsp = g_thr[i].is_main ? g_pause_frame : g_thr[i].ctx.Rsp;
        describe(g_thr[i].is_main ? 0 : g_thr[i].ctx.Rip, rip, sizeof rip);
        blog("  #%d tid=%lu '%s' depth=0x%llx rip=%s GuestThread=0x%llx tcb=0x%llx tls=0x%llx%s", g_thr[i].order, g_thr[i].tid, g_thr[i].name, g_thr[i].stackbase - rsp, rip, g_thr[i].gt, g_thr[i].tcb, g_thr[i].tls_addr, g_thr[i].is_main ? " (stopped in hook)" : "");
    }
    resume_all(); release_main();
}
/* ---- L3 research: values in guest memory that point into the host ----
 * Runs while the game runs (read only; counts are approximate). Every aligned qword of the VMAs and
 * eboot RW pages that falls into a committed host region (not guest memory, not the backing view)
 * is classified: known objects (GuestThread, TCB, TLS block, tracked mutex/cond/attr handles,
 * VideoOut port and labels, equeues) or by host region (module image, other low-area allocation,
 * heap/private, mapped). */
typedef struct { uint64_t lo, hi; int kind; char name[40]; uint64_t refs; } HReg;
enum { HK_IMAGE, HK_PRIVATE, HK_MAPPED, HK_LOWAREA };
static int cmp_hreg(const void *a, const void *b) { uint64_t x = ((const HReg *)a)->lo, y = ((const HReg *)b)->lo; return x < y ? -1 : x > y; }
static void hostrefs(void) {
    double t0 = now_ms();
    static HReg reg[65536]; int nr = 0;
    static Vma vm[MAX_VMAS]; int nv = read_vmas(vm, MAX_VMAS);
    uint64_t bb = (uint64_t)backing(), bb_end = bb; for (int i = 0; i < nv; ++i) if (bb + vm[i].phys + (vm[i].end - vm[i].start) > bb_end) bb_end = bb + vm[i].phys + (vm[i].end - vm[i].start);
    uint64_t eboot_end = 0x805700000ull; /* eboot segments; the rest of [0x800000000, low_next) is host allocations */
    for (uint64_t a = 0x10000; a < 0x7FFFFFFF0000ull && nr < 65536;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void *)a, &m, sizeof m)) break;
        uint64_t lo = (uint64_t)m.BaseAddress, hi = lo + m.RegionSize;
        int guest = 0;
        for (int i = 0; i < nv; ++i) if (overlaps(lo, hi, vm[i].start, vm[i].end)) guest = 1;
        if (overlaps(lo, hi, bb, bb_end) || overlaps(lo, hi, 0x800000000ull, eboot_end)) guest = 1;
        if (m.State == MEM_COMMIT && !guest) {
            HReg *r = &reg[nr++]; memset(r, 0, sizeof *r); r->lo = lo; r->hi = hi;
            r->kind = m.Type == MEM_IMAGE ? HK_IMAGE : m.Type == MEM_MAPPED ? HK_MAPPED : (lo >= 0x800000000ull && lo < low_next()) ? HK_LOWAREA : HK_PRIVATE;
            if (m.Type == MEM_IMAGE) { for (int k = 0; k < g_nmods; ++k) if (lo >= g_mods[k].base && lo < g_mods[k].base + g_mods[k].size) strncpy_s(r->name, sizeof r->name, g_mods[k].name, _TRUNCATE); }
        }
        a = hi;
    }
    qsort(reg, nr, sizeof *reg, cmp_hreg);
    /* known objects */
    uint64_t live[1024]; int nl = live_guest_threads(live, 1024);
    static uint64_t tcbs[1024], tls_lo[1024], tls_hi[1024];
    for (int i = 0; i < nl; ++i) { tcbs[i] = *(uint64_t *)(live[i] + GT_TCB); tls_lo[i] = *(uint64_t *)(live[i] + GT_TLS_BLOCK); tls_hi[i] = tcbs[i] + 256; }
    uint8_t *port = vo_port();
    int32_t eqk[16]; uint8_t *eqs[16]; int neq = vo_equeues(eqk, eqs, 16);
    enum { C_GT, C_TCB, C_TLS, C_SYNC, C_KEPT, C_VO, C_EQ, C_IMG, C_LOW, C_PRIV, C_MAP, C_N };
    static const char *cn[C_N] = {"GuestThread*", "TCB", "TLS block", "tracked mutex/cond/attr", "kept (destroyed) objects", "VideoOut port/labels", "EqueueInternal*",
                                  "module image", "other low-area host alloc", "host heap/private", "mapped file"};
    uint64_t cnt[C_N] = {0}, per_vma[MAX_VMAS][3]; memset(per_vma, 0, sizeof per_vma);
    uint64_t sample[C_N][4]; int ns[C_N] = {0};
    int nimg = 0; for (int i = 0; i < g_nreg; ++i) if (g_reg[i].addr < 0x900000000ull) ++nimg;
    /* scan: VMAs through the backing view, then the eboot RW regions collected by the last save/load */
    for (int vi = 0; vi <= nv; ++vi) {
        uint64_t gstart, len; const uint64_t *p;
        if (vi < nv) { if (vm[vi].kind == 1) continue; gstart = vm[vi].start; len = vm[vi].end - vm[vi].start; p = (const uint64_t *)(bb + vm[vi].phys); }
        else { gstart = 0x800000000ull; len = 0; p = NULL; }
        for (int part = 0; part < (vi < nv ? 1 : g_nreg); ++part) {
            if (vi == nv) { if (g_reg[part].addr >= 0x900000000ull) continue; gstart = g_reg[part].addr; len = g_reg[part].size; p = (const uint64_t *)g_reg[part].addr; }
            for (uint64_t k = 0; k < len / 8; ++k) {
                uint64_t v;
                __try { v = p[k]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
                if (v < 0x10000 || v >= 0x7FFFFFFF0000ull) continue;
                int lo = 0, hi = nr - 1, f = -1;
                while (lo <= hi) { int mid = (lo + hi) / 2; if (reg[mid].lo <= v) { f = mid; lo = mid + 1; } else hi = mid - 1; }
                if (f < 0 || v >= reg[f].hi) continue;
                int c = -1;
                for (int i = 0; i < nl && c < 0; ++i) { if (v == live[i]) c = C_GT; else if (v == tcbs[i]) c = C_TCB; else if (v >= tls_lo[i] && v < tls_hi[i]) c = C_TLS; }
                if (c < 0 && port && v >= (uint64_t)port && v < (uint64_t)port + 0x400) c = C_VO;
                for (int i = 0; i < neq && c < 0; ++i) if (v == (uint64_t)eqs[i]) c = C_EQ;
                if (c < 0) {
                    uint32_t h = tab_hash(v);
                    for (uint32_t j = h; g_tab[j].key; j = (j + 1) & (TAB_SIZE - 1)) if (g_tab[j].key == v) { c = C_SYNC; break; }
                }
                if (c < 0) { LONG nk = g_nkept < KEPT_CAP ? g_nkept : KEPT_CAP; for (LONG i = 0; i < nk; ++i) if (g_keptlist[i].key == v) { c = C_KEPT; break; } }
                if (c < 0) c = reg[f].kind == HK_IMAGE ? C_IMG : reg[f].kind == HK_LOWAREA ? C_LOW : reg[f].kind == HK_MAPPED ? C_MAP : C_PRIV;
                ++cnt[c]; ++reg[f].refs;
                if (vi < nv) ++per_vma[vi][c <= C_EQ ? 0 : c == C_IMG ? 1 : 2];
                if (ns[c] < 4) sample[c][ns[c]++] = gstart + k * 8;
            }
        }
    }
    blog("HOSTREFS: %d host regions; scan %.0f ms; %d guest threads, %d equeues", nr, now_ms() - t0, nl, neq);
    for (int c = 0; c < C_N; ++c)
        blog("  %-28s %8llu refs  e.g. at 0x%llx 0x%llx 0x%llx", cn[c], cnt[c], ns[c] > 0 ? sample[c][0] : 0, ns[c] > 1 ? sample[c][1] : 0, ns[c] > 2 ? sample[c][2] : 0);
    for (int i = 0; i < nv; ++i) if (per_vma[i][0] + per_vma[i][1] + per_vma[i][2])
        blog("  VMA 0x%llx-0x%llx kind %d: known objects %llu, module pointers %llu, other host %llu", vm[i].start, vm[i].end, vm[i].kind, per_vma[i][0], per_vma[i][1], per_vma[i][2]);
    for (int pass = 0; pass < 12; ++pass) { /* top host regions by references */
        int best = -1; for (int i = 0; i < nr; ++i) if (reg[i].refs && (best < 0 || reg[i].refs > reg[best].refs)) best = i;
        if (best < 0) break;
        blog("  top region 0x%llx-0x%llx (%llu KB) %s %s: %llu refs", reg[best].lo, reg[best].hi, (reg[best].hi - reg[best].lo) >> 10,
             reg[best].kind == HK_IMAGE ? "image" : reg[best].kind == HK_LOWAREA ? "low-area" : reg[best].kind == HK_MAPPED ? "mapped" : "private", reg[best].name, reg[best].refs);
        reg[best].refs = 0;
    }
}
static void dump_threads(void) {
    if (!enumerate_threads()) return;
    blog("DUMP: %d guest threads", g_nthr);
    for (int i = 0; i < g_nthr; ++i) {
        GThread *t = &g_thr[i];
        if (SuspendThread(t->h) == (DWORD)-1) continue;
        t->ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(t->h, &t->ctx)) log_thread_state(wait_name(t->ctx.Rip) ? "wait" : "busy", t);
        ResumeThread(t->h);
    }
    close_threads();
}

/* ---- crash notes: a fault in guest code near address 0 or at a non-user address (the port then ends the process with its own
 * report): registers and the memory they point at go to the log first. Registered after the port's
 * handler, so it runs first; everything else (the port's page tracking) passes through untouched. */
static volatile LONG g_crash_noted;
static int g_crash_notes;   /* env BBSTATE_CRASH_NOTES=1 */
static void dump_at(const char *nm, uint64_t a) {
    char b[600]; int n = 0;
    MEMORY_BASIC_INFORMATION m;
    if (a < 0x10000 || !VirtualQuery((void *)a, &m, sizeof m) || m.State != MEM_COMMIT) { blog("CRASH   %s=0x%llx (not readable)", nm, a); return; }
    for (int i = -4; i < 16 && n < 560; ++i) {
        uint64_t v = 0; __try { v = ((uint64_t *)a)[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        n += _snprintf_s(b + n, sizeof b - n, _TRUNCATE, "%s%llx", i == 0 ? " |" : " ", v);
    }
    blog("CRASH   %s=0x%llx:%s", nm, a, b);
}
/* Small minidump (stacks, registers, memory they point to) next to bbstate.log: logs\crash_<time>.dmp. */
typedef BOOL (WINAPI *MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, int, void *, void *, void *);
static void write_minidump(EXCEPTION_POINTERS *ep) {
    MiniDumpWriteDumpFn fn = (MiniDumpWriteDumpFn)GetProcAddress(LoadLibraryA("dbghelp.dll"), "MiniDumpWriteDump");
    if (!fn) { blog("CRASH: no dbghelp.dll, no dump"); return; }
    SYSTEMTIME st; GetLocalTime(&st);
    char p[MAX_PATH]; _snprintf_s(p, sizeof p, _TRUNCATE, "%s\\logs\\crash_%04d%02d%02d_%02d%02d%02d.dmp", g_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    HANDLE f = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { blog("CRASH: cannot create %s", p); return; }
    struct { DWORD tid; EXCEPTION_POINTERS *ep; BOOL client; } ei = {GetCurrentThreadId(), ep, FALSE};
    /* MiniDumpWithIndirectlyReferencedMemory 0x40 | MiniDumpWithThreadInfo 0x1000 | MiniDumpWithUnloadedModules 0x20 */
    BOOL ok = fn(GetCurrentProcess(), GetCurrentProcessId(), f, 0x40 | 0x1000 | 0x20, &ei, NULL, NULL);
    CloseHandle(f);
    blog("CRASH: dump %s %s", p, ok ? "written" : "failed");
}
static LONG CALLBACK crash_note(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *r = ep->ExceptionRecord; CONTEXT *c = ep->ContextRecord;
    /* the port's page tracking faults only inside guest memory (backing view 0x680000000.., image, heaps) */
    uint64_t fa = r->NumberParameters >= 2 ? (uint64_t)r->ExceptionInformation[1] : 0;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || (fa >= 0x680000000ull && fa < 0x800000000000ull)) return EXCEPTION_CONTINUE_SEARCH;
    if (c->Rip < 0x800000000ull || c->Rip >= 0x805400000ull || InterlockedExchange(&g_crash_noted, 1)) return EXCEPTION_CONTINUE_SEARCH;
    if (g_crash_notes > 1) write_minidump(ep);
    char d[96]; describe(c->Rip, d, sizeof d);
    blog("CRASH: %s reads 0x%llx (thread %lu); last load: %s", d, (uint64_t)r->ExceptionInformation[1], GetCurrentThreadId(), g_last_load_note);
    const char *nm[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    for (int i = 0; i < 16; ++i) dump_at(nm[i], (&c->Rax)[i]);
    __try { uint64_t o = c->Rbx - 0x60, own = *(uint64_t *)(o + 0x18); dump_at("[rbx-0x60]", o); dump_at("owner [o+0x18]", own); dump_at("pool [own+0x28]", *(uint64_t *)(own + 0x28)); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---- watcher: death, PlayerIns, guest threads, VMAs ---- */
static struct { int hp, inited; uint64_t ins; uint64_t thr[1024]; int nthr; int nvma; uint64_t vsig; double death_t; int death_pending; } W;
static uint64_t vma_sig(int *n) {
    static Vma v[MAX_VMAS];
    *n = read_vmas(v, MAX_VMAS);
    uint64_t s = 1469598103934665603ull;
    for (int i = 0; i < *n; ++i) { s = (s ^ v[i].start) * 1099511628211ull; s = (s ^ v[i].end) * 1099511628211ull; }
    return s;
}
static void watch_tick(void) {
    Player p = read_player();
    double t = now_ms();
    if (!W.inited) { W.hp = p.hp; W.ins = p.ins; W.nthr = live_guest_threads(W.thr, 1024); W.vsig = vma_sig(&W.nvma); W.inited = 1; return; }
    if (p.ins != W.ins) { if (O.watch) blog("WATCH PlayerIns 0x%llx -> 0x%llx (hp %d)", W.ins, p.ins, p.hp); W.ins = p.ins; if (p.ins) { if (p.ins == g_load_ins) g_load_ins = 0; else g_player_appear_t = t; } }
    if (p.ok && p.hp != W.hp) {
        if (p.hp <= 0 && W.hp > 0) {
            blog("WATCH DEATH: hp %d -> %d at pos (%.1f, %.1f, %.1f)", W.hp, p.hp, p.pos[0], p.pos[1], p.pos[2]);
            W.death_t = t; W.death_pending = 1;
        } else if (O.watch && W.hp <= 0 && p.hp > 0) blog("WATCH hp %d -> %d (alive again, %.0f ms after death)", W.hp, p.hp, W.death_t ? t - W.death_t : 0.0);
        W.hp = p.hp;
    } else if (!p.ok && W.hp) { if (O.watch) blog("WATCH player not readable (was hp %d)", W.hp); W.hp = 0; }
    static unsigned tick;
    if (++tick % 5) return; /* threads and VMAs every 50 ms */
    uint64_t cur[1024]; int n = live_guest_threads(cur, 1024);
    if (n >= 0 && (n != W.nthr || memcmp(cur, W.thr, n * 8))) {
        if (O.watch) {
            char add[1500], gone[1500]; int a = 0, g = 0, na = 0, ng = 0; add[0] = gone[0] = 0;
            for (int i = 0; i < n; ++i) { int f = 0; for (int j = 0; j < W.nthr; ++j) if (W.thr[j] == cur[i]) f = 1; if (!f) { ++na; if (a < 1400) a += _snprintf_s(add + a, sizeof add - a, _TRUNCATE, " %.15s", (char *)(cur[i] + GT_NAME)); } }
            for (int j = 0; j < W.nthr; ++j) { int f = 0; for (int i = 0; i < n; ++i) if (W.thr[j] == cur[i]) f = 1; if (!f) { ++ng; if (g < 1400) g += _snprintf_s(gone + g, sizeof gone - g, _TRUNCATE, " %.15s", (char *)(W.thr[j] + GT_NAME)); } }
            blog("WATCH threads %d -> %d%s: +%d [%s ] -%d [%s ]", W.nthr, n, W.death_t ? "" : "", na, add, ng, gone);
            if (W.death_t) blog("WATCH   (%.0f ms after the last death)", t - W.death_t);
        }
        memcpy(W.thr, cur, n * 8); W.nthr = n;
    }
    int nv; uint64_t vs = vma_sig(&nv);
    if (vs != W.vsig) { if (O.watch) blog("WATCH VMAs changed: %d -> %d%s", W.nvma, nv, W.death_t ? " (after a death)" : ""); W.vsig = vs; W.nvma = nv; }
}

/* ---- options ---- */
static int parse_key(const char *s, Key *k) { return hk_parse(s, k); }
static const char *k_default_ini =
    "; Practice tool settings (most of them are changed in the overlay: Home)\r\n"
    "[keys]\r\n"
    "; F5 save to the active slot, F9 load it; next / previous slot: no key until one is set in the overlay.\r\n"
    "; The port uses F7 (mouse), F8 (reload input.ini), Insert (its menu).\r\n"
    "save=F5\r\nload=F9\r\nnext_slot=none\r\nprev_slot=none\r\n"
    "[overlay]\r\n"
    "toggle_key=Home\r\nnotifications=1\r\npause_game=1\r\nopacity=85\r\nsize=100\r\n"
    "[practice]\r\n"
    "immortal_key=none\r\nstamina_key=none\r\nconsumables_key=none\r\nstealth_key=none\r\nsilent_key=none\r\nsave_position_key=none\r\nload_position_key=none\r\n"
    "[slots]\r\n"
    "; folder for slots and the page pool (relative to this file or absolute)\r\n"
    "dir=slots\r\n"
    "; active slot 1..10\r\n"
    "active=1\r\n"
    "[options]\r\n"
    "; load the active slot as soon as the player dies\r\n"
    "autoload_on_death=0\r\n"
    "; sound signals: 1 on, 0 off; volume 1..100\r\n"
    "beeps=0\r\nbeep_volume=25\r\n"
    "; threads for hashing during the pause / for background compression\r\n"
    "fast_threads=12\r\nbg_threads=3\r\n"
    "; log deaths, thread and memory layout changes\r\n"
    "watch_log=1\r\n"
    "; load a slot saved in another run of the game, in another location or before a death; 1 on, 0 off\r\n"
    "cross_process_load=1\r\n"
    "; frames to wait for every game thread to stand where it stood at the save (such loads / loads in the same run)\r\n"
    "cross_process_tries=300\r\nsame_process_tries=60\r\n"
    "; F5 refuses when the slots' drive has less free space (MB)\r\n"
    "min_free_mb=2048\r\n"
    "; 1: a slot saved less than 8 s after the player appeared loads only in the run that saved it\r\n"
    "refuse_early_slots=0\r\n"
    "; game save files copied into each slot (fallback plan); empty: the port's own save folder\r\n"
    "save_files=\r\n";
static void load_options(void) {
    char v[MAX_PATH];
    _snprintf_s(O.ini, MAX_PATH, _TRUNCATE, "%s\\practice-tool.ini", g_dir);
    {   /* settings of the launch.py days: bbstate.ini next to the DLL */
        char old[MAX_PATH]; _snprintf_s(old, sizeof old, _TRUNCATE, "%s\\bbstate.ini", g_dir);
        if (GetFileAttributesA(O.ini) == INVALID_FILE_ATTRIBUTES && GetFileAttributesA(old) != INVALID_FILE_ATTRIBUTES) CopyFileA(old, O.ini, TRUE);
    }
    if (GetFileAttributesA(O.ini) == INVALID_FILE_ATTRIBUTES) {
        HANDLE f = CreateFileA(O.ini, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, k_default_ini, (DWORD)strlen(k_default_ini), &w, NULL); CloseHandle(f); }
    }
#define KEY(field, name, def) GetPrivateProfileStringA("keys", name, def, v, sizeof v, O.ini); if (!parse_key(v, &O.field)) { blog("ini: bad key '%s' for %s, using %s", v, name, def); parse_key(def, &O.field); }
    KEY(save, "save", "F5"); KEY(load, "load", "F9"); KEY(next, "next_slot", "none"); KEY(prev, "prev_slot", "none");
    GetPrivateProfileStringA("slots", "dir", "slots", v, sizeof v, O.ini);
    if (v[1] == ':' || v[0] == '\\') strncpy_s(O.slots_dir, MAX_PATH, v, _TRUNCATE);
    else _snprintf_s(O.slots_dir, MAX_PATH, _TRUNCATE, "%s\\%s", g_dir, v);
    O.active = GetPrivateProfileIntA("slots", "active", 1, O.ini);
    if (O.active < 1 || O.active > STORE_MAX_SLOTS) O.active = 1;
    O.autoload = GetPrivateProfileIntA("options", "autoload_on_death", 0, O.ini);
    O.beeps = GetPrivateProfileIntA("options", "beeps", 0, O.ini);
    O.beep_volume = GetPrivateProfileIntA("options", "beep_volume", 25, O.ini);
    O.fast_threads = GetPrivateProfileIntA("options", "fast_threads", 12, O.ini);
    O.bg_threads = GetPrivateProfileIntA("options", "bg_threads", 3, O.ini);
    O.watch = GetPrivateProfileIntA("options", "watch_log", 1, O.ini);
    g_l3_enabled = GetPrivateProfileIntA("options", "cross_process_load", 1, O.ini);
    O.l3_tries = GetPrivateProfileIntA("options", "cross_process_tries", 300, O.ini);
    O.l1_tries = GetPrivateProfileIntA("options", "same_process_tries", 60, O.ini);
    O.min_free_mb = GetPrivateProfileIntA("options", "min_free_mb", 2048, O.ini);
    g_early_ok = !GetPrivateProfileIntA("options", "refuse_early_slots", 0, O.ini);
    GetPrivateProfileStringA("options", "save_files", "", O.save_files, sizeof O.save_files, O.ini);
    blog("options: save=%s load=%s next=%s prev=%s, slots %s, active %d, autoload_on_death %d, beeps %d (vol %d), threads %d/%d",
         O.save.text, O.load.text, O.next.text, O.prev.text, O.slots_dir, O.active, O.autoload, O.beeps, O.beep_volume, O.fast_threads, O.bg_threads);
}
static void do_delete(int slot) {
    g_cur_op = BBS_OP_DELETE; g_cur_slot = slot; api_begin(BBS_OP_DELETE, slot);
    int had = store_delete(slot);
    if (g_virt) vs_slot_forget(slot);
    blog("delete slot %d: %s", slot, had ? "ok" : "was empty");
    if (had) api_end(BBS_OP_DELETE, slot, 1, "", 0); else api_end(BBS_OP_DELETE, slot, 0, "the slot is empty", 0);
}
static void set_active(int s) {
    O.active = s;
    char v[8]; _snprintf_s(v, sizeof v, _TRUNCATE, "%d", s);
    WritePrivateProfileStringA("slots", "active", v, O.ini);
    blog("active slot %d (%s)", s, store_slot_exists(s) == 1 ? "saved" : store_slot_exists(s) == 2 ? "saved, compressing" : "empty");
    play(SND_SLOT1 + s - 1);
}

/* ---- bbs_core.h: what api.c needs from the engine ---- */
int core_slot_state(int slot) { return store_slot_exists(slot); }
int core_slot_damaged(int slot) { return store_slot_damaged(slot); }
void core_slot_dir(int slot, char *out, size_t n) { _snprintf_s(out, n, _TRUNCATE, "%s\\slot%02d", O.slots_dir, slot); }
int core_active(void) { return O.active; }
void core_set_active(int slot) { set_active(slot); }
void core_get_options(BbsOptions *o) {
    o->autoload_on_death = O.autoload; o->beeps = O.beeps; o->beep_volume = O.beep_volume; o->cross_process_load = g_l3_enabled; o->min_free_mb = O.min_free_mb;
}
static void ini_int(const char *sec, const char *key, int v) { char b[16]; _snprintf_s(b, sizeof b, _TRUNCATE, "%d", v); WritePrivateProfileStringA(sec, key, b, O.ini); }
void core_set_options(const BbsOptions *o) {
    int vol = o->beep_volume < 1 ? 1 : o->beep_volume > 100 ? 100 : o->beep_volume, rebuild = vol != O.beep_volume;
    O.autoload = !!o->autoload_on_death; O.beeps = !!o->beeps; O.beep_volume = vol; g_l3_enabled = !!o->cross_process_load; O.min_free_mb = o->min_free_mb < 0 ? 0 : o->min_free_mb;
    ini_int("options", "autoload_on_death", O.autoload); ini_int("options", "beeps", O.beeps); ini_int("options", "beep_volume", O.beep_volume);
    ini_int("options", "cross_process_load", g_l3_enabled); ini_int("options", "min_free_mb", O.min_free_mb);
    if (rebuild) { PlaySoundA(NULL, NULL, 0); for (int i = 0; i < SND_COUNT; ++i) { free(g_snd[i]); g_snd[i] = NULL; } init_sounds(); }
    blog("options set: autoload_on_death %d, beeps %d (volume %d), cross_process_load %d, min_free_mb %d", O.autoload, O.beeps, O.beep_volume, g_l3_enabled, O.min_free_mb);
}
/* ---- ov.h: what the overlay needs from the engine and the port ---- */
static volatile uint8_t g_ov_capture, g_ov_capture_pad;   /* read by the CapturesInput / pad stubs */
void core_ov_capture(int mouse, int game) { g_ov_capture = (uint8_t)(mouse != 0); g_ov_capture_pad = (uint8_t)(game != 0); }
void core_ov_hold(int on) {
    if (!g_hooked) on = 0;
    if (InterlockedExchange(&g_ov_hold, on) != on) { if (g_ev_wake) SetEvent(g_ev_wake); blog("overlay: game %s", on ? "held at the next frame" : "released"); }
}
int core_ov_held(void) { return g_ov_held != 0; }
void core_ov_redraw(void) {   /* only while the render thread waits and no flip is left: the presenter is idle */
    if (!g_ov_held || g_state != 0 || !vo_flips_idle()) return;   /* not during a save/load pause */
    uint8_t *drv = *(uint8_t **)(g_base + RVA_VO_DRIVER);
    if (drv) ((void (*)(void *))(g_base + RVA_DRAW_LAST_FRAME))(drv);
}
const char *core_key_text(int which) { const Key *k = which == 0 ? &O.save : which == 1 ? &O.load : which == 2 ? &O.next : &O.prev; return k->text; }
int core_set_key(int which, const char *text) {
    static const char *const names[4] = {"save", "load", "next_slot", "prev_slot"};
    Key k; if (which < 0 || which > 3 || !parse_key(text, &k)) return 0;
    *(which == 0 ? &O.save : which == 1 ? &O.load : which == 2 ? &O.next : &O.prev) = k;
    WritePrivateProfileStringA("keys", names[which], k.text, O.ini);
    blog("key %s = %s", names[which], k.text);
    return 1;
}
void core_slots_dir(char *out, int n) { strncpy_s(out, n, O.slots_dir, _TRUNCATE); }
/* BbOverlay::CapturesInput (port menu open or text entry) also answers "yes" while our panel is open:
 * the port then releases the mouse and shows the cursor itself. pad_read_state asks through
 * bbgpu_overlay_captures_input, hooked apart: with "pause game" off the game keeps its pad and keyboard
 * while the panel is open (the mouse stays with the panel; mouse buttons reach the game only when captured). */
static const char *install_capture_hook(void) {
    uint8_t *fn = g_base + RVA_CAPTURES_INPUT;
    static const uint8_t pro[7] = {0x55, 0x48, 0x89, 0xe5, 0x0f, 0xb6, 0x05};
    if (memcmp(fn, pro, 7) || fn[11] != 0x0a || fn[12] != 0x05 || fn[17] != 0x24 || fn[18] != 0x01) return "BbOverlay::CapturesInput differs";
    uint64_t menu_open = (uint64_t)(fn + 11) + *(int32_t *)(fn + 7), text_entry = (uint64_t)(fn + 17) + *(int32_t *)(fn + 13), mine = (uint64_t)&g_ov_capture;
    uint8_t *st = alloc_near(g_base, 4096);
    if (!st) return "no memory near the exe";
    uint8_t *p = st;
    *p++ = 0x48; *p++ = 0xb8; memcpy(p, &menu_open, 8); p += 8;        /* mov rax, &menu_open */
    *p++ = 0x0f; *p++ = 0xb6; *p++ = 0x00;                             /* movzx eax, byte [rax] */
    *p++ = 0x48; *p++ = 0xb9; memcpy(p, &text_entry, 8); p += 8;       /* mov rcx, &text_entry_active */
    *p++ = 0x0a; *p++ = 0x01;                                          /* or al, [rcx] */
    *p++ = 0x48; *p++ = 0xb9; memcpy(p, &mine, 8); p += 8;             /* mov rcx, &g_ov_capture */
    *p++ = 0x0a; *p++ = 0x01;                                          /* or al, [rcx] */
    *p++ = 0x83; *p++ = 0xe0; *p++ = 0x01;                             /* and eax, 1 */
    *p++ = 0xc3;
    uint8_t *pad = g_base + RVA_PAD_CAPTURE, *q = st + 128;
    static const uint8_t pad_pro[10] = {0x55, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8d, 0x6c, 0x24, 0x20};
    int pad_ok = !memcmp(pad, pad_pro, 10) && pad[10] == 0xe8 && pad + 15 + *(int32_t *)(pad + 11) == fn;
    uint64_t mine_pad = (uint64_t)&g_ov_capture_pad;
    *q++ = 0x48; *q++ = 0xb8; memcpy(q, &menu_open, 8); q += 8;        /* the same with g_ov_capture_pad */
    *q++ = 0x0f; *q++ = 0xb6; *q++ = 0x00;
    *q++ = 0x48; *q++ = 0xb9; memcpy(q, &text_entry, 8); q += 8;
    *q++ = 0x0a; *q++ = 0x01;
    *q++ = 0x48; *q++ = 0xb9; memcpy(q, &mine_pad, 8); q += 8;
    *q++ = 0x0a; *q++ = 0x01;
    *q++ = 0x83; *q++ = 0xe0; *q++ = 0x01;
    *q++ = 0xc3;
    FlushInstructionCache(GetCurrentProcess(), st, 4096);
    if (!patch_jmp5(fn, st)) return "VirtualProtect failed";
    if (!pad_ok) return "pad check differs: game input stays blocked while the panel is open";
    if (!patch_jmp5(pad, st + 128)) return "VirtualProtect failed (pad)";
    return NULL;
}

/* command "slots": what the overlay would show (bbs_list, bbs_status, the last events) */
static void slots_report(void) {
    BbsSlotInfo si[BBS_SLOTS]; int n = bbs_list(si, BBS_SLOTS); BbsStatus bs; bbs_status(&bs);
    blog("slots: active %d, state %d, queued %d; last op %d slot %d ok %d pause %.0f ms '%s'", bs.active_slot, bs.state, bs.queued, bs.last_op, bs.last_slot, bs.last_ok, bs.last_pause_ms, bs.last_reason);
    for (int i = 0; i < n; ++i) if (si[i].used)
        blog("  slot %d: '%s' | %s (%s) | %s | play %llu s | level %d | echoes %u | insight %d | hp %d/%d | pos %.1f %.1f %.1f | thumb %s%s%s", si[i].slot, si[i].name, si[i].location, si[i].map, si[i].created,
             si[i].play_time_ms / 1000, si[i].level, si[i].echoes, si[i].insight, si[i].hp, si[i].hp_max, si[i].pos[0], si[i].pos[1], si[i].pos[2], si[i].thumb[0] ? si[i].thumb : "-",
             si[i].same_run_only ? " | this run only" : "", si[i].damaged ? " DAMAGED" : "");
    BbsEvent ev[64]; int ne = bbs_events(0, ev, 64);
    for (int i = ne > 8 ? ne - 8 : 0; i < ne; ++i) blog("  event #%llu type %d slot %d: %s", ev[i].seq, ev[i].type, ev[i].slot, ev[i].text);
}

/* ---- commands ---- */
static int handle_command(char *line) {
    while (*line == ' ' || *line == '\t') ++line;
    char *e = line + strlen(line);
    while (e > line && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ')) *--e = 0;
    if (!*line) return 0;
    blog("command: %s", line);
    int arg = atoi(line + strcspn(line, " ")) ;
    int slot = arg >= 1 && arg <= STORE_MAX_SLOTS ? arg : O.active;
    if (!strncmp(line, "save", 4)) do_save(slot);
    else if (!strncmp(line, "load", 4)) do_load(slot, "command");
    else if (!strncmp(line, "slot ", 5) && arg >= 1 && arg <= STORE_MAX_SLOTS) set_active(arg);
    else if (!strncmp(line, "delete ", 7) && arg >= 1) do_delete(arg);
    else if (!strncmp(line, "rename ", 7) && arg >= 1) { const char *nm = line + 7; while (*nm && *nm != ' ') ++nm; while (*nm == ' ') ++nm; blog("rename slot %d to '%s': %s", arg, nm, bbs_rename(arg, nm) ? "ok" : "no such slot"); }
    else if (!strcmp(line, "slots")) slots_report();
    else if (!strcmp(line, "refill")) { char m[160]; int r = prac_refill(m, sizeof m); blog("refill: %d %s", r, m); }
    else if (!strncmp(line, "autoload", 8)) { O.autoload = arg ? 1 : 0; blog("autoload_on_death %d", O.autoload); }
    else if (!strncmp(line, "spin ", 5)) { vs_set_spin(arg); blog("vsync spin %d", arg); }
    else if (!strncmp(line, "l3 ", 3)) { g_l3_enabled = arg ? 1 : 0; blog("cross-process load %d", g_l3_enabled); }
    else if (!strncmp(line, "clockskew ", 10)) { g_tsc_off += (int64_t)arg * (int64_t)g_tsc_hz; g_mono_off_ns += (int64_t)arg * 1000000000ll; blog("test: guest TSC and monotonic clock jump %d s forward (now +%.0f s / +%.0f s)", arg, g_tsc_hz ? (double)g_tsc_off / g_tsc_hz : 0.0, g_mono_off_ns / 1e9); }
    else if (!strncmp(line, "clockfix ", 9)) { g_clockfix = arg; blog("loads move the guest clocks forward: %d", arg); }
    else if (!strncmp(line, "failwrites ", 11)) { store_fail_writes(arg); blog("test: pack writes fail %d", arg); }
    else if (!strncmp(line, "minfree ", 8)) { O.min_free_mb = arg; blog("min_free_mb %d", arg); }
    else if (!strncmp(line, "noinv ", 6)) { g_noinv = arg; blog("no GPU invalidation after a load %d", arg); }
    else if (!strncmp(line, "verifyload ", 11)) { g_verify = arg; blog("verify after load %d", arg); }
    else if (!strncmp(line, "xstats ", 7)) { g_xstats = arg; blog("translation statistics %d", g_xstats); }
    else if (!strncmp(line, "deadwin ", 8)) { g_deadwin = arg; blog("dead stack window written back on loads %d", arg); }
    else if (!strncmp(line, "ajmstate ", 9)) { g_ajm_on = arg; blog("audio decoder state restored on loads %d", arg); }
    else if (!strncmp(line, "l1stacks ", 9)) { g_l1_stacks = arg; blog("same-process stacks mode %d", g_l1_stacks); }
    else if (!strncmp(line, "remap", 5)) { g_remap_enabled = arg ? 1 : 0; blog("experimental respawn remap %d", g_remap_enabled); }
    else if (!strncmp(line, "watch", 5)) { O.watch = arg ? 1 : 0; blog("watch_log %d", O.watch); }
    else if (!strcmp(line, "flush")) { double t = now_ms(); store_flush(); blog("flush: background work done after %.0f ms", now_ms() - t); }
    else if (!strcmp(line, "threads")) threads_report();
    else if (!strcmp(line, "census")) census();
    else if (!strncmp(line, "earlyok ", 8)) { g_early_ok = arg; blog("test: early slots load in other runs %d", arg); }
    else if (!strncmp(line, "hostq", 5)) {   /* hostq N: N samples 5 ms apart while the game runs */
        int n = arg > 0 ? arg : 200, busy = 0; HostQ mx; memset(&mx, 0, sizeof mx); uint64_t gap = 0, fen = 0, ops = 0, hc = 0, df = 0, bc = 0;
        for (int i = 0; i < n; ++i) {
            HostQ q = host_q(); busy += host_q_busy(&q);
            if (q.tick_cur > q.tick_done + 1 && q.tick_cur - q.tick_done - 1 > gap) gap = q.tick_cur - q.tick_done - 1;
            if (q.pfences > fen) fen = q.pfences; if (q.ops > ops) ops = q.ops; if (q.hc_issued - q.hc_done > hc) hc = q.hc_issued - q.hc_done;
            if (q.df_issued - q.df_done > df) df = q.df_issued - q.df_done; if (q.bc_tasks > bc) bc = q.bc_tasks;
            if (i == 0 || i == n - 1) { char a[400]; host_q_str(&q, a, sizeof a); blog("HOSTQ sample %d: %s", i, a); }
            Sleep(5);
        }
        blog("HOSTQ: %d samples, %d busy; max: GPU ticks in flight %llu, pending ops %llu, host copies %llu, deferred signals %llu, pending fences %llu, pool tasks %llu", n, busy, gap, ops, hc, df, fen, bc);
    }
    else if (!strcmp(line, "stackrefs")) stackrefs();
    else if (!strcmp(line, "ovstats")) { char b[300]; ov_stats(b, sizeof b); blog("%s", b); }
    else if (!strncmp(line, "ovshot ", 7)) blog("ovshot: %s", ov_screenshot(line + 7) ? "written" : "FAILED (overlay not drawing?)");
    else if (!strncmp(line, "ovopen ", 7)) ov_command(line + 7);
    else if (!strcmp(line, "vsync")) {
        static VsCounts last; static double last_t; VsCounts c; vs_counts(&c); double t = now_ms(), dt = last_t ? (t - last_t) / 1000 : 0;
        static const char *nm[] = {"create", "destroy", "lock", "unlock", "trylock", "cwait", "csignal", "rwread", "rwwrite", "rwunlock", "contended", "static_init", "errors", "retarget", "swait", "ssignal"};
        char b[900]; int n = 0;
        for (int k = 0; k < 16; ++k) n += _snprintf_s(b + n, sizeof b - n, _TRUNCATE, " %s=%llu%s", nm[k], c.calls[k], "");
        blog("vsync: virt=%d live mutex %u cond %u attr %u rwlock %u sema %u, high water %u; totals:%s", g_virt, vs_live(VS_MUTEX), vs_live(VS_COND), vs_live(VS_MATTR), vs_live(VS_RWLOCK), vs_live(VS_SEMA), vs_high_water(), b);
        if (dt > 0) { n = 0; for (int k = 0; k < 16; ++k) n += _snprintf_s(b + n, sizeof b - n, _TRUNCATE, " %s=%.0f", nm[k], (c.calls[k] - last.calls[k]) / dt); blog("vsync: per second over %.1f s:%s", dt, b); }
        last = c; last_t = t;
    } else if (!strcmp(line, "bench")) {
        double ours, orig; vs_bench(g_base, RVA_ORIG_MUTEX_LOCK, RVA_ORIG_MUTEX_UNLOCK, RVA_INIT_MUTEX, &ours, &orig);
        blog("bench: uncontended lock+unlock: vsync %.1f ns, runtime original %.1f ns", ours, orig);
    }
    else if (!strcmp(line, "dump")) dump_threads();
    else if (!strcmp(line, "hostrefs")) { if (!g_nreg) { enumerate_threads(); collect_regions(); close_threads(); } hostrefs(); }
    else if (!strcmp(line, "beep")) { play(SND_SAVED); Sleep(400); play(SND_LOADED); Sleep(400); play(SND_REFUSED); Sleep(500); play(SND_SLOT1 + 6); }
    else if (!strcmp(line, "status")) {
        uint32_t ns; int wr, sl; int idle = gpu_idle(&ns, &wr, &sl);
        Player p = read_player();
        blog("status: hooked=%d active slot %d autoload %d; player ok=%d hp %d/%d pos (%.2f, %.2f, %.2f); gpu idle=%d", g_hooked, O.active, O.autoload, p.ok, p.hp, p.hpmax, p.pos[0], p.pos[1], p.pos[2], idle);
        blog("kept: mutex %ld, cond %ld, mutexattr %ld, rwlock %ld, sema %ld, threadattr %ld; freed normally: mutex %ld, cond %ld, attr %ld; tracked live %ld; gen %ld",
             g_kept[0], g_kept[1], g_kept[2], g_kept[3], g_kept[4], g_kept[5], g_freed[0] + g_freed[1], g_freed[2] + g_freed[3], g_freed[4] + g_freed[5], g_tab_count, g_gen);
        store_status();
        { int used = 0; AcquireSRWLockShared(&g_fs_lock); for (int i = 0; i <= FS_NSLOTS; ++i) used += g_fs[i].used; ReleaseSRWLockShared(&g_fs_lock);
          blog("fixed stacks: %s; %ld threads started on one (%ld probe steps), %ld failed, %d in use; canonical depth %ld, missed %ld",
               g_fs_on ? "on" : "off", g_fs_count, g_fs_probe, g_fs_fail, used, canon_done, canon_missed); }
        mem_note("status");
    } else if (!strcmp(line, "unload")) return 1;
    else blog("unknown command");
    blog("command done: %s", line);
    return 0;
}
static int poll_cmd_file(void) {
    char path[MAX_PATH], buf[4096];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\cmd.txt", g_dir);
    HANDLE f = CreateFileA(path, GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD got = 0;
    ReadFile(f, buf, sizeof buf - 1, &got, NULL);
    CloseHandle(f);
    DeleteFileA(path);
    buf[got] = 0;
    int quit = 0;
    for (char *ctx = NULL, *l = strtok_s(buf, "\n", &ctx); l && !quit; l = strtok_s(NULL, "\n", &ctx)) quit = handle_command(l);
    return quit;
}
static int game_focused(void) {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}
static int key_pressed(const Key *k, int *prev) { return hk_pressed(k, prev); }

static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    g_worker_tid = GetCurrentThreadId();
    QueryPerformanceFrequency(&g_qpf);
    g_base = (uint8_t *)GetModuleHandleA(NULL);
    blog("---- bbstate loaded: pid %lu, exe base %p, worker tid %lu%s%s, data folder %s", GetCurrentProcessId(), g_base, g_worker_tid,
         g_proxy ? " (pdh.dll proxy)" : "", g_early ? " (early: waiting for the game)" : "", g_dir);
    if (!g_supported) {
        static char ini[MAX_PATH]; _snprintf_s(ini, sizeof ini, _TRUNCATE, "%s\\practice-tool.ini", g_dir);
        blog("bb-probe.exe sha256 %s is not bbport 1.9.5 (%s): savestates and practice tools are off, the overlay only explains that", g_exe_sha, BBPROBE_195_SHA256);
        OvConfig oc = {0, "This version of bbport is not supported yet: the practice tool works with bbport 1.9.5 only. "
                          "The game itself runs as usual. To remove the tool, delete pdh.dll and the practice-tool folder from the game's bin folder.", g_dir, ini};
        ov_start(&oc);
        return 0;
    }
    if (g_early) game_output_to_file();
    if (g_early) {
        double tw = now_ms();
        if (!wait_game_started()) { blog("early: the game did not start in 10 min: doing nothing"); return 0; }
        VsCounts vc; vs_counts(&vc);
        blog("early: game running after %.1f s; resolver calls %ld, vsync live: mutex %u cond %u attr %u rwlock %u; canonical stacks: %ld threads, %ld already deeper",
             (now_ms() - tw) / 1000, g_resolve_calls, vs_live(VS_MUTEX), vs_live(VS_COND), vs_live(VS_MATTR), vs_live(VS_RWLOCK), canon_done, canon_missed);
    }
    FILETIME c, x, k, u; GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u);
    g_proc_start = ((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime;
    load_options();
    init_sounds();
    api_init();
    measure_tsc();
    g_ntqit = (NtQIT)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    init_modules();
    g_dirty = (Range *)VirtualAlloc(NULL, g_dirty_cap * sizeof(Range), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_tblob = (uint8_t *)VirtualAlloc(NULL, g_tblob_cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_tls = (uint8_t *)VirtualAlloc(NULL, g_tls_cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_oblob_cap = 16 + (uint64_t)(TAB_SIZE + KEPT_CAP) * sizeof(ORec);
    g_oblob = (uint8_t *)VirtualAlloc(NULL, g_oblob_cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (g_virt) g_vsbuf = (uint8_t *)VirtualAlloc(NULL, VS_CAPTURE_MAX, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (g_virt) g_htls = (uint8_t *)VirtualAlloc(NULL, g_htls_cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_ajm = (uint8_t *)VirtualAlloc(NULL, g_ajm_cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_ev_paused = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_ev_resume = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!store_init(O.slots_dir, O.fast_threads, O.bg_threads, blog)) { blog("store not available: doing nothing"); return 0; }
    gate_init();
    if (!g_gate) { blog("no memory for the gate: doing nothing"); store_shutdown(); return 0; }
    refresh_modules();
    if (!install_hooks(1)) { blog("hooks not installed: doing nothing"); store_shutdown(); return 0; }
    install_keep(1);
    /* ini [options] crash_dump=1: register notes in bbstate.log + logs\crash_*.dmp (env BBSTATE_CRASH_NOTES=1: notes only) */
    { char cn[8] = ""; GetEnvironmentVariableA("BBSTATE_CRASH_NOTES", cn, sizeof cn); g_crash_notes = cn[0] == '1'; }
    if (GetPrivateProfileIntA("options", "crash_dump", 0, O.ini)) g_crash_notes = 2;
    if (g_crash_notes) { AddVectoredExceptionHandler(1, crash_note); blog("crash notes on%s", g_crash_notes > 1 ? " (with a dump file)" : ""); }
    blog("ready: %s save, %s load, %s/%s slot; active slot %d; cmd.txt in %s", O.save.text, O.load.text, O.next.text, O.prev.text, O.active, g_dir);
    g_ev_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    { OvConfig oc = {1, "", g_dir, O.ini}; ov_start(&oc); }
    mem_note("start");
    int ps = 0, pl = 0, pn = 0, pp = 0;
    for (unsigned tick = 0;; ++tick) {
        Sleep(10);
        watch_tick();
        if (W.death_pending) {
            W.death_pending = 0;
            if (O.autoload && store_slot_exists(O.active)) {
                blog("AUTOLOAD: player died, loading slot %d (%.0f ms after HP reached 0)", O.active, now_ms() - W.death_t);
                if (do_load(O.active, "death")) { W.hp = read_player().hp; }
            }
        }
        if (game_focused()) {   /* the keys go through the same queue as the overlay's requests; while the overlay */
            int ok = !ov_keys_blocked();   /* takes typing or a new hotkey, presses are read and dropped */
            int s = key_pressed(&O.save, &ps), l = key_pressed(&O.load, &pl), n = key_pressed(&O.next, &pn), p = key_pressed(&O.prev, &pp);
            if (ok && s) bbs_save(0);
            if (ok && l) bbs_load(0);
            if (ok && n) bbs_set_active(O.active % STORE_MAX_SLOTS + 1);
            if (ok && p) bbs_set_active((O.active + STORE_MAX_SLOTS - 2) % STORE_MAX_SLOTS + 1);
        }
        for (int op, sl; api_take(&op, &sl);) {
            if (op == BBS_OP_SAVE) do_save(sl);
            else if (op == BBS_OP_LOAD) do_load(sl, "key");
            else if (op == BBS_OP_DELETE) do_delete(sl);
        }
        if (tick % 10 == 0 && poll_cmd_file()) break;
        if (tick % 10 == 0) { int d = store_take_dropped(); if (d) { blog("SAVE slot %d LOST: its pages could not be written to disk (disk full?)", d); play(SND_REFUSED); api_event(BBS_EV_LOST, d, BBS_OP_SAVE, "Slot %d lost: the disk is full?", d); } }
    }
    install_hooks(0);
    install_keep(0);
    Sleep(300); /* a caller that read the slot just before may still be in a thunk */
    store_shutdown();
    if (g_dirty) VirtualFree(g_dirty, 0, MEM_RELEASE);
    VirtualFree(g_tblob, 0, MEM_RELEASE); VirtualFree(g_oblob, 0, MEM_RELEASE);
    close_threads();
    PlaySoundA(NULL, NULL, 0);
    blog("unloading (kept host objects stay allocated: the game may still use them)");
    CloseHandle(g_log); g_log = INVALID_HANDLE_VALUE;
    FreeLibraryAndExitThread(g_self, 0);
    return 0;
}

int proxy_init(void);
int sha256_file(const char *path, char out[65]);
/* The data folder: BBSTATE_DIR, else <exe folder>\practice-tool when loaded as the pdh.dll proxy or when
 * that folder exists (launch.py then uses the same slots), else the DLL's own folder (development). */
static void pick_dir(HINSTANCE h) {
    char exe[MAX_PATH], pt[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    char *s = strrchr(exe, '\\'); if (s) *s = 0;
    _snprintf_s(pt, sizeof pt, _TRUNCATE, "%s\\practice-tool", exe);
    if (GetEnvironmentVariableA("BBSTATE_DIR", g_dir, sizeof g_dir) && g_dir[0]) return;
    if (g_proxy || GetFileAttributesA(pt) != INVALID_FILE_ATTRIBUTES) { CreateDirectoryA(pt, NULL); strncpy_s(g_dir, sizeof g_dir, pt, _TRUNCATE); return; }
    GetModuleFileNameA(h, g_dir, sizeof g_dir);
    s = strrchr(g_dir, '\\'); if (s) *s = 0;
    size_t n = strlen(g_dir);
    if (n > 4 && !_stricmp(g_dir + n - 4, "\\bin")) g_dir[n - 4] = 0; /* launch.py / inject.py load copies from bin\ */
}
static void open_log(void) {
    char d[MAX_PATH], path[MAX_PATH], old[MAX_PATH];
    _snprintf_s(d, sizeof d, _TRUNCATE, "%s\\logs", g_dir);
    CreateDirectoryA(d, NULL);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\bbstate.log", d);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &a) && (a.nFileSizeHigh || a.nFileSizeLow > (32u << 20))) {   /* keep one older log */
        _snprintf_s(old, sizeof old, _TRUNCATE, "%s\\bbstate.old.log", d);
        MoveFileExA(path, old, MOVEFILE_REPLACE_EXISTING);
    }
    g_log = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS, 0, NULL);
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
#ifdef BBS_SHIFT_TEST
        g_shift_pad[0] = (uint8_t)shift_code((int)GetCurrentProcessId());
#endif
        DisableThreadLibraryCalls(h);
        char me[MAX_PATH], exe[MAX_PATH];
        GetModuleFileNameA(h, me, sizeof me); GetModuleFileNameA(NULL, exe, sizeof exe);
        const char *mn = strrchr(me, '\\'), *en = strrchr(exe, '\\');
        mn = mn ? mn + 1 : me; en = en ? en + 1 : exe;
        if (!_stricmp(mn, "pdh.dll")) {
            g_proxy = 1;
            proxy_init();                              /* the real pdh.dll: always, whoever loaded us */
            if (_stricmp(en, "bb-probe.exe")) return TRUE;   /* another program of the port's bin folder: only the proxy */
        }
        char mx[64]; _snprintf_s(mx, sizeof mx, _TRUNCATE, "Local\\bbstate_%lu", GetCurrentProcessId());
        HANDLE one = CreateMutexA(NULL, FALSE, mx);
        if (one && GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(one); return TRUE; }   /* the pdh.dll proxy already runs here (launch.py on top of it) */
        pick_dir(h);
        InitializeCriticalSection(&g_log_cs);
        QueryPerformanceFrequency(&g_qpf);
        open_log();
        g_base = (uint8_t *)GetModuleHandleA(NULL);
        double th = now_ms();
        g_supported = sha256_file(exe, g_exe_sha) && !strcmp(g_exe_sha, BBPROBE_195_SHA256);
        { char t[4] = ""; if (GetEnvironmentVariableA("BBSTATE_TEST_UNSUPPORTED", t, sizeof t) && t[0] == '1') g_supported = 0; }   /* test of the refusal path */
        blog("==== %s loaded into %s (pid %lu): %s, sha256 checked in %.0f ms; data folder %s", mn, en, GetCurrentProcessId(),
             g_supported ? "bbport 1.9.5" : "UNSUPPORTED exe", now_ms() - th, g_dir);
        ov_hook_vulkan(h, blog);                       /* any version: the overlay can at least say why it is off */
        if (g_supported) {
            early_attach();
            const char *ce = install_capture_hook();
            blog("overlay: input capture %s", ce ? ce : "hooked (BbOverlay::CapturesInput)");
        }
        HANDLE t = CreateThread(NULL, 1 << 20, worker, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
