/* bbs_core.h: between api.c (the overlay's interface, bbs_api.h) and bbstate.c (the savestate engine). */
#pragma once
#include <stddef.h>
#include "bbs_api.h"

/* bbstate.c */
int core_slot_state(int slot);              /* 0 empty, 1 saved, 2 saved and compressing */
int core_slot_damaged(int slot);
void core_slot_dir(int slot, char *out, size_t n);
int core_active(void);
void core_set_active(int slot);             /* remembered in bbstate.ini, slot sound */
void core_get_options(BbsOptions *o);
void core_set_options(const BbsOptions *o); /* applied and written to bbstate.ini */

/* api.c */
void api_init(void);
int api_take(int *op, int *slot);           /* worker thread: the next queued request (1) or none (0) */
void api_begin(int op, int slot);           /* a save/load/delete starts, whatever asked for it (keys, overlay, cmd.txt) */
void api_end(int op, int slot, int ok, const char *reason, double pause_ms);
void api_event(int type, int slot, int op, const char *fmt, ...);
