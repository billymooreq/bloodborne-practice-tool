/* keys.c: hotkey names <-> virtual keys (keys.h). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "keys.h"

static const struct { int vk; const char *name; } k_names[] = {
    {VK_HOME, "Home"}, {VK_END, "End"}, {VK_PRIOR, "PageUp"}, {VK_NEXT, "PageDown"}, {VK_INSERT, "Insert"}, {VK_DELETE, "Delete"},
    {VK_PAUSE, "Pause"}, {VK_SCROLL, "ScrollLock"}, {VK_NUMPAD0, "Num0"}, {VK_NUMPAD1, "Num1"}, {VK_NUMPAD2, "Num2"}, {VK_NUMPAD3, "Num3"},
    {VK_NUMPAD4, "Num4"}, {VK_NUMPAD5, "Num5"}, {VK_NUMPAD6, "Num6"}, {VK_NUMPAD7, "Num7"}, {VK_NUMPAD8, "Num8"}, {VK_NUMPAD9, "Num9"},
    {VK_MULTIPLY, "Num*"}, {VK_ADD, "Num+"}, {VK_SUBTRACT, "Num-"}, {VK_DIVIDE, "Num/"}, {VK_DECIMAL, "Num."},
    {VK_OEM_MINUS, "-"}, {VK_OEM_PLUS, "="}, {VK_OEM_4, "["}, {VK_OEM_6, "]"}, {VK_OEM_1, ";"}, {VK_OEM_7, "'"}, {VK_OEM_COMMA, ","},
    {VK_OEM_PERIOD, "."}, {VK_OEM_2, "/"}, {VK_OEM_5, "\\"}, {VK_OEM_3, "`"}, {VK_UP, "Up"}, {VK_DOWN, "Down"}, {VK_LEFT, "Left"}, {VK_RIGHT, "Right"},
    {VK_SPACE, "Space"}, {VK_TAB, "Tab"}, {VK_BACK, "Backspace"}, {VK_RETURN, "Enter"}, {VK_XBUTTON1, "Mouse4"}, {VK_XBUTTON2, "Mouse5"},
};
const char *hk_vk_name(int vk) {
    static char buf[8][8]; static int bi;
    if (vk >= VK_F1 && vk <= VK_F24) { char *b = buf[bi++ & 7]; sprintf_s(b, 8, "F%d", vk - VK_F1 + 1); return b; }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) { char *b = buf[bi++ & 7]; b[0] = (char)vk; b[1] = 0; return b; }
    for (size_t i = 0; i < sizeof k_names / sizeof *k_names; ++i) if (k_names[i].vk == vk) return k_names[i].name;
    return "";
}
void hk_format(int vk, int shift, int ctrl, int alt, char *out, size_t n) {
    if (!vk || !*hk_vk_name(vk)) { strncpy_s(out, n, "none", _TRUNCATE); return; }
    _snprintf_s(out, n, _TRUNCATE, "%s%s%s%s", ctrl ? "Ctrl+" : "", alt ? "Alt+" : "", shift ? "Shift+" : "", hk_vk_name(vk));
}
int hk_parse(const char *s, HotKey *k) {
    memset(k, 0, sizeof *k);
    while (*s == ' ') ++s;
    for (;;) {
        if (!_strnicmp(s, "shift+", 6)) { k->shift = 1; s += 6; }
        else if (!_strnicmp(s, "ctrl+", 5)) { k->ctrl = 1; s += 5; }
        else if (!_strnicmp(s, "alt+", 4)) { k->alt = 1; s += 4; }
        else break;
    }
    if (!*s || !_stricmp(s, "none")) { strncpy_s(k->text, sizeof k->text, "none", _TRUNCATE); return 1; }
    if ((s[0] == 'F' || s[0] == 'f') && isdigit((unsigned char)s[1]) && atoi(s + 1) >= 1 && atoi(s + 1) <= 24) k->vk = VK_F1 + atoi(s + 1) - 1;
    else if (strlen(s) == 1 && isalnum((unsigned char)s[0])) k->vk = toupper((unsigned char)s[0]);
    else for (size_t i = 0; i < sizeof k_names / sizeof *k_names; ++i) if (!_stricmp(s, k_names[i].name)) k->vk = k_names[i].vk;
    if (!k->vk) return 0;
    hk_format(k->vk, k->shift, k->ctrl, k->alt, k->text, sizeof k->text);
    return 1;
}
int hk_pressed(const HotKey *k, int *prev) {
    if (!k->vk) return 0;
    SHORT ks = GetAsyncKeyState(k->vk);
    int down = (ks & 0x8000) != 0, hit = (down && !*prev) || (!down && (ks & 1)); /* bit 0: a press shorter than the poll */
    *prev = down;
    if (!hit) return 0;
    int sh = GetAsyncKeyState(VK_SHIFT) < 0, ct = GetAsyncKeyState(VK_CONTROL) < 0, al = GetAsyncKeyState(VK_MENU) < 0;
    return sh == k->shift && ct == k->ctrl && al == k->alt;
}
