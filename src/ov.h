/* ov.h: the in-game overlay (C++ side: ov_vk.cpp draws into the port's frames, ov_ui.cpp is the panel).
 * Interface for bbstate.c (C). The Vulkan hooks do not depend on the port version; everything that
 * touches the game goes through bbstate.c (core_ov_*) or practice.c and is off when the version differs. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*OvLogFn)(const char *fmt, ...);
void ov_hook_vulkan(void *self_module, OvLogFn log);   /* DllMain: patches vulkan-1.dll's exports */

typedef struct {
    int supported;           /* the tool found its parts in bb-probe.exe and the savestate engine runs */
    const char *why;         /* shown when not supported */
    const char *dir;         /* practice-tool folder (slots, teleport points, logs) */
    const char *ini;         /* settings file */
    const char *note;        /* NULL, or a lasting warning in the header (a bbport version not tested with the tool) */
} OvConfig;
void ov_start(const OvConfig *c);                      /* starts the overlay thread (hotkeys, pause, data) */
int ov_thumbnail(uint8_t *bgr, int timeout_ms);        /* META_THUMB_W x H bottom-up BGR (meta.h) of the next presented frame; 1 ok */
int ov_keys_blocked(void);                             /* typing in the overlay or waiting for a new hotkey */
int ov_is_open(void);
int ov_screenshot(const char *path);                   /* next drawn frame with the overlay -> BMP (command "ovshot FILE") */
void ov_command(const char *what);                     /* tests: "open TAB" / "close" (command "ovopen ...") */
void ov_stats(char *out, int n);                      /* our cost per present since the last call (command "ovstats") */

/* bbstate.c, for the overlay */
void core_ov_capture(int mouse, int game);   /* the port's "menu captures input": mouse free + cursor / game input (pad, keyboard) off */
void core_ov_hold(int on);           /* stop the game at the next frame boundary (render thread waits in the frame hook) */
int core_ov_held(void);              /* the render thread is waiting */
void core_ov_redraw(void);           /* present the last frame again (VideoOut DrawLastFrame) while the game stands */
const char *core_key_text(int which);                /* 0 save, 1 load, 2 next slot, 3 previous slot */
int core_set_key(int which, const char *text);       /* parsed and written to the ini; 1 ok */
void core_slots_dir(char *out, int n);

#ifdef __cplusplus
}
#endif
