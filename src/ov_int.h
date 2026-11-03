/* ov_int.h: between ov_vk.cpp (Vulkan, input, textures) and ov_ui.cpp (the panel, the overlay thread). */
#pragma once
#include <stdint.h>
#include "imgui.h"

/* ov_ui.cpp */
bool ui_wants_frame(void);              /* panel open or a toast on screen: the present hook draws */
bool ui_wants_input(void);              /* panel open: mouse/keyboard events go to ImGui */
void ui_setup_fonts(ImGuiIO &io);       /* once, when the ImGui context is made */
void ui_build(float w, float h);        /* between ImGui::NewFrame and ImGui::Render, on the present thread */
void ui_on_key(int vk);                 /* a key went down while the panel is open (hotkey assignment) */
extern OvLogFn ov_log;

/* ov_vk.cpp: thumbnails and pictures as textures (uploaded by the present hook) */
enum { OVTEX_SLOT0 = 0, OVTEX_MINI0 = 12, OVTEX_COUNT = 24 };   /* MINI: small copy for the slot list (no mipmaps) */
void ovtex_set(int id, const uint8_t *rgba, int w, int h);   /* copied; uploaded with the next drawn frame; NULL removes */
ImTextureID ovtex_id(int id, int *w, int *h);                 /* 0 until uploaded */
void ov_window_size(int *w, int *h);    /* swapchain extent of the last drawn frame */
HWND ov_game_window(void);
double ov_fps(void);                    /* presents per second over the last second */
