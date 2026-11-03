/* keys.h: hotkey names ("F5", "Shift+F6", "Home", "none") <-> virtual keys; polling with exact modifiers (keys.c). */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { int vk, shift, ctrl, alt; char text[32]; } HotKey;
int hk_parse(const char *s, HotKey *k);                                 /* 1 ok ("none"/"" = no key) */
void hk_format(int vk, int shift, int ctrl, int alt, char *out, size_t n);
const char *hk_vk_name(int vk);                                         /* "" for keys that cannot be hotkeys */
int hk_pressed(const HotKey *k, int *prev);                             /* went down since the last call, modifiers as given */
#ifdef __cplusplus
}
#endif
