/* store.c: savestate slots on disk.
 * <dir>/pool/pack_NNNNNN.dat  append-only records {u32 'BBPG', u32 csize|flags, H128 hash, data}
 * <dir>/pool/index.bin        hash -> (pack, offset, csize); rebuilt from the packs if missing
 * <dir>/slotNN/               pages.bin (zstd: regions + XXH3-128 of every page, {0,0} = zero page),
 *                             layout.bin, threads.bin, hostobjs.bin, metadata.json, fallback/
 * A page is stored once however many slots use it (reference counts are rebuilt from the slots at
 * start); zero pages are not stored. A save hashes guest memory while the game is paused and copies
 * only pages the pool does not have into a staging buffer; a background thread compresses them
 * (zstd -1 per page) into the packs. Deleting or overwriting a slot drops pages nobody else uses;
 * packs that are mostly dead get compacted. */
#pragma once
#include <stdint.h>

#define STORE_MAX_SLOTS 10
typedef struct { uint64_t addr; uint8_t *host; uint64_t size; } SRegion; /* size: whole pages */
typedef struct {
    uint64_t pages, zero, known, staged, staged_bytes;
    double hash_ms, copy_ms;
} SaveStats;
typedef struct {
    uint64_t pages, changed, zeroed, from_staging, from_disk, disk_bytes, gpu_overwritten;
    double ms, compare_ms, before_ms;
} LoadStats;
typedef void (*DirtyFn)(uint64_t addr, uint64_t len);
typedef void (*LogFn)(const char *fmt, ...);

int store_init(const char *dir, int fast_threads, int bg_threads, LogFn log);
void store_shutdown(void);
/* Game paused: hashes the regions, stages pages the pool lacks. Takes the store lock and keeps it
 * until store_save_commit. 0 = failed (lock released). */
int store_save_pages(const SRegion *r, int nr, SaveStats *st);
/* Game running again: puts the staged pages in the index, writes the slot's files (blobs may be
 * NULL), drops the slot's previous contents, queues compression. Releases the lock. */
int store_save_commit(int slot, const char *meta_json, const void *layout, uint32_t layout_len,
                      const void *threads, uint32_t threads_len, const void *objs, uint32_t objs_len,
                      const char *fallback_dir);
void store_save_abort(void);       /* instead of commit: drops the staged pages, releases the lock */
/* Before the pause: reads the slot's page table (and layout blob, malloc'd, caller frees). */
int store_load_prepare(int slot, void **layout, uint32_t *layout_len);
/* Game paused: finds the pages that differ from the slot, calls before_write() for each changed
 * range (GPU cache invalidation), writes the slot's pages, then dirty() for each changed range. */
int store_load_pages(const SRegion *r, int nr, DirtyFn before_write, DirtyFn dirty, LoadStats *st);
void store_load_done(void);
uint64_t store_load_verify(const SRegion *r, int nr, uint64_t *first, int nfirst);   /* debug: pages that differ from the slot now */
void store_load_prefetch(void);   /* after prepare: the slot's packs into the file cache (loads that write most pages) */
void store_load_prepare_stats(double *ms, uint64_t *bytes);   /* mapping + prefetch of the slot's packs in prepare */
void *store_read_threads(int slot, uint64_t *len);   /* threads.bin decompressed (free it) */
int store_delete(int slot);
int store_slot_exists(int slot);   /* 0 none, 1 complete, 2 pages still being compressed */
void store_status(void);
uint64_t store_ram_bytes(void);    /* memory the store holds right now */
uint64_t store_ram_peak(void);
void store_flush(void);            /* wait for the background work */
/* fn(ctx, i, t) for i in [0, items) on the fast pool (t < store_threads()); the caller takes part. */
int store_parallel(void (*fn)(void *ctx, uint64_t item, int t), void *ctx, uint64_t items);
int store_threads(void);
uint64_t store_free_bytes(void);   /* free space on the slots' drive */
int store_take_dropped(void);      /* a slot dropped because its pages could not be written (0: none since the last call) */
int store_slot_damaged(int slot);  /* pages of the slot the pool check could not read back (0: intact or not checked yet) */
void store_fail_writes(int on);    /* test: pack writes fail as on a full disk */
