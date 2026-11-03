/* practice.h: practice tools on the game's memory (bbport 1.9.5 / Bloodborne 1.09 only). practice.c */
#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { PR_IMMORTAL, PR_STAMINA, PR_ITEMS, PR_STEALTH, PR_SILENT, PR_COUNT };
typedef struct {
    int ok;                  /* a hunter is in the world */
    uint32_t map_id;
    float pos[3], yaw;       /* yaw in radians */
    int hp, hp_max, insight, level;
    uint32_t echoes;
} PracPlayer;
int prac_read(PracPlayer *p);
int prac_flag(int which);                      /* current state of a toggle (read from the game) */
int prac_set_flag(int which, int on);          /* the user's choice; kept through loads, deaths and area changes (prac_tick) */
void prac_tick(void);                          /* overlay thread, ~10 ms: puts the toggles back to the user's choice */
int prac_set_echoes(uint32_t v);
int prac_set_insight(int v);
int prac_refill(char *msg, size_t n);          /* blood vials and quicksilver bullets to the maximum; message for the user.
                                                  2: an item the inventory lacks is added on the next frame (prac_refill_poll) */
int prac_refill_poll(char *msg, size_t n, char *note, size_t nn);   /* overlay thread: 1/-1 the pending refill finished/failed, 0 nothing */
void prac_frame(void);                         /* render thread at the frame boundary (bbs_pre): runs a pending refill */
int prac_teleport(uint32_t map_id, const float pos[3], float yaw, char *why, size_t n);
#ifdef __cplusplus
}
#endif
