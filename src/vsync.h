/* vsync: the guest's mutexes, condition variables, mutex attributes and rwlocks, implemented in the
 * DLL (early mode). Guest cells hold stable handles (table index + type + generation) instead of host
 * pointers, so the whole synchronization state is plain data that a slot can carry to another process. */
#pragma once
#include <stdint.h>

#define VS_TAG      0x5B5B000000000000ull   /* non-canonical: a host dereference faults at once */
#define VS_TAGMASK  0xFFFF000000000000ull
enum { VS_FREE = 0, VS_MUTEX = 1, VS_COND = 2, VS_MATTR = 3, VS_RWLOCK = 4, VS_SEMA = 5, VS_NTYPES = 6 };
/* kernel semaphores: the guest holds 32-bit ids VS_SEMA_TAG | (gen & 0x3f) << 24 | index */
#define VS_SEMA_TAG 0x40000000u

typedef struct {
    uint64_t calls[16];       /* by operation, summed over threads (see vs_counts) */
} VsCounts;
enum { VC_CREATE, VC_DESTROY, VC_LOCK, VC_UNLOCK, VC_TRYLOCK, VC_CWAIT, VC_CSIGNAL, VC_RWREAD, VC_RWWRITE, VC_RWUNLOCK,
       VC_CONTENDED, VC_STATIC_INIT, VC_ERRORS, VC_RETARGET, VC_SWAIT, VC_SSIGNAL };

int vs_init(void (*log)(const char *, ...));
int vs_enabled(void);
/* Wrapper (System V entry) for an import NID, or 0. */
uint64_t vs_lookup(const char *nid);
/* The wrappers in a fixed order (NEW ONES ONLY AT THE END: slots keep gate addresses); returns the count. */
int vs_entries(void (**out)(void), int cap);
void vs_counts(VsCounts *out);
uint32_t vs_live(int type);
void vs_set_spin(int n);
/* What thread tid waits for inside vsync now: 0 nothing, 1 mutex, 2 cond, 3 rwlock (c1/c2 its guest cells),
 * 4 semaphore (c1 id, c2 tokens needed). */
int vs_thread_wait(unsigned long tid, uint64_t *c1, uint64_t *c2);   /* pause iterations before a contended lock sleeps */
uint32_t vs_high_water(void);
/* Table snapshot for a slot into buf (no heap use: runs while guest threads are suspended):
 * header, entries [0, high water), current holds (diagnostics). Returns the length, 0 if cap is short. */
uint64_t vs_capture(uint8_t *buf, uint64_t cap);
#define VS_CAPTURE_MAX (sizeof(uint64_t) * 8 + (1ull << 20) * 64 + 4096ull * 32)
/* Puts a captured table back (all guest threads stopped). Same process: merge (the slot's objects
 * return at their indices, objects of now stay, nobody wakes). Other process: the slot's table replaces
 * this one; see restore_replace. ext: memory not restored (stacks, registers) that may hold handles. */
typedef struct { uint64_t *p; uint64_t n; } VsRange;   /* qwords outside restored memory that may hold handles */
int vs_restore(const uint8_t *b, uint64_t len, const VsRange *ext, int next, int same_process,
               const unsigned long *tid_a, const unsigned long *tid_b, int ntid, char *note, size_t nn);
/* tid_a[i] (saving process) -> tid_b[i] (now): threads whose guest stack state was transplanted from
 * the slot; they get the slot's lock holds. vs_set_thread_wait: the cells a waiting thread now waits on. */
void vs_set_thread_wait(unsigned long tid, uint64_t c1, uint64_t c2);
/* Table lineage (merge only within one), per-slot object lists and quarantine hygiene. */
uint64_t vs_table_id(void);
uint64_t vs_slot_table_id(const uint8_t *b, uint64_t len);
void vs_slot_commit(int slot, int ok);
void vs_slot_forget(int slot);
uint32_t vs_hygiene(uint32_t *kept);
/* type (> 0) if v is a live handle, minus its type if it has the tag but names no live object, 0 otherwise */
int vs_handle_state(uint64_t v);
/* 1 if v looks like a vsync handle; type out */
int vs_is_handle(uint64_t v, int *type);
/* Measures ns per uncontended lock+unlock pair: ours and the runtime's original functions. */
void vs_bench(uint8_t *exe_base, uint64_t rva_lock, uint64_t rva_unlock, uint64_t rva_init, double *ours, double *orig);
