/* bbs_api.h: the savestate module's interface for the in-game overlay (same DLL, called directly).
 * Every function is thread-safe and returns at once: save, load and delete are queued to the DLL's
 * worker thread (they pause the game and must not run on the render thread, where an overlay draws);
 * results come back as events and in the status. Hotkeys (F5/F9/F6/Shift+F6) and cmd.txt go through
 * the same queue. Slots are numbered 1..BBS_SLOTS; slot 0 in a call means the active slot. */
#pragma once
#include <stdint.h>

#define BBS_SLOTS 10

typedef struct {
    int slot;
    int used;                /* 0 empty, 1 saved, 2 saved (pages still being compressed: loads fine) */
    int damaged;             /* pages the pool check could not read back (load refuses) */
    char name[64];           /* given with bbs_rename, else "<location> <hh:mm>" */
    char hunter[48];         /* the character's name (UTF-8), "" in slots of older builds */
    char location[64];       /* English place name as the game shows it ("Central Yharnam"); "" if unknown */
    char map[16];            /* map id, e.g. "m24_01" */
    char created[20];        /* local time of the save, "2026-11-02 12:34:56" */
    uint64_t play_time_ms;   /* the game's play time at the save */
    int level, insight, hp, hp_max;
    int same_run_only;       /* saved right after the player appeared (area still loading): loads only in the game run that saved it */
    uint32_t echoes;         /* Blood Echoes held */
    float pos[3];
    char thumb[260];         /* thumbnail (24-bit BMP, BBS_THUMB_W x BBS_THUMB_H) or "" */
} BbsSlotInfo;
#define BBS_THUMB_W 256
#define BBS_THUMB_H 144

enum { BBS_READY, BBS_SAVING, BBS_LOADING, BBS_DELETING };
enum { BBS_OP_NONE, BBS_OP_SAVE, BBS_OP_LOAD, BBS_OP_DELETE };
typedef struct {
    int state;               /* BBS_READY / SAVING / LOADING / DELETING (the request being worked on) */
    int queued;              /* requests waiting behind it */
    int active_slot;
    int last_op, last_slot, last_ok;   /* the last finished save/load/delete */
    char last_reason[256];   /* why it was refused or failed ("" when it worked) */
    double last_pause_ms;    /* how long the game stood still */
    uint64_t last_event;     /* seq of the newest event */
} BbsStatus;

enum { BBS_EV_SAVED = 1, BBS_EV_LOADED, BBS_EV_REFUSED, BBS_EV_DELETED, BBS_EV_RENAMED, BBS_EV_ACTIVE, BBS_EV_LOST };
typedef struct {
    uint64_t seq;            /* 1, 2, 3, ... */
    int type, slot, op;      /* op: BBS_OP_* the event answers (REFUSED) */
    double time_ms;          /* QueryPerformanceCounter time */
    char text[200];          /* short English text for a notification ("Saved to slot 3", reason of a refusal) */
} BbsEvent;

typedef struct {
    int autoload_on_death;   /* load the active slot when the player dies */
    int beeps, beep_volume;  /* sound signals 0/1, volume 1..100 */
    int cross_process_load;  /* slots of another run / location / before a death */
    int min_free_mb;         /* F5 refuses below this free disk space */
} BbsOptions;

int  bbs_save(int slot);                        /* queued: 1, refused at once (bad slot, queue full): 0 */
int  bbs_load(int slot);
int  bbs_delete(int slot);
int  bbs_rename(int slot, const char *name);    /* "" restores the default name; 1 ok */
int  bbs_set_active(int slot);                  /* 1 ok; remembered in bbstate.ini */
int  bbs_active(void);
int  bbs_slot_info(int slot, BbsSlotInfo *out); /* 1 if the slot is used */
int  bbs_list(BbsSlotInfo *out, int cap);       /* all slots 1..min(cap, BBS_SLOTS) in order; returns how many filled */
void bbs_status(BbsStatus *out);
int  bbs_events(uint64_t after_seq, BbsEvent *out, int cap);   /* events newer than after_seq, oldest first (the last 64 are kept) */
void bbs_get_options(BbsOptions *out);
int  bbs_set_options(const BbsOptions *in);     /* applied at once and written to bbstate.ini */
