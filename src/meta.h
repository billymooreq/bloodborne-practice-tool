/* meta.h: slot metadata (meta.c) */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define META_THUMB_W 256
#define META_THUMB_H 144
#define META_THUMB_BYTES (META_THUMB_W * META_THUMB_H * 3)   /* 24-bit rows, bottom-up, width*3 is a multiple of 4 */
typedef struct {
    char location[64], map[16], created[20];
    char hunter[48];     /* character name (UTF-8) */
    uint32_t map_id, echoes;
    uint64_t play_time_ms;
    int level, insight, hp, hp_max;
    int same_run_only;   /* saved while the area was still loading: loads only in the run that saved it */
    float pos[3];
} SlotMeta;

const char *meta_place(uint32_t map_id);            /* English place name of a map id ("" if unknown) */
int meta_read_game(SlotMeta *m);                    /* from the running game (game threads running); 0: no player */
void meta_default_name(const SlotMeta *m, char *out, size_t n);
int meta_capture(uint8_t *bgr);                     /* META_THUMB_BYTES of the game window; 0: not visible / black */
int meta_write(const char *slot_dir, const SlotMeta *m, const char *name, const uint8_t *thumb);
int meta_read(const char *slot_dir, SlotMeta *m, char *name, size_t nn, char *thumb, size_t tn);
int meta_rename(const char *slot_dir, const char *name);
