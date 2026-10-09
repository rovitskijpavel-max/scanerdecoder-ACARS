#ifndef XPCOMPAT_H
#define XPCOMPAT_H
/* Мелочи для Windows XP: на ней нет шрифта Segoe UI и в Tahoma нет символа ▶ (U+25B6) - берём то, что есть. */
#include <windows.h>

static inline int xp_is_xp(void) {
    return (int)LOBYTE(LOWORD(GetVersion())) < 6; /* Windows XP/2003 - версия 5.x */
}
static inline const WCHAR *xp_ui_face(void) {
    return xp_is_xp() ? L"Tahoma" : L"Segoe UI";
}
/* символ "воспроизвести": в Tahoma (XP) есть ► U+25BA, а ▶ U+25B6 нет */
static inline const WCHAR *xp_play_glyph(void) {
    return xp_is_xp() ? L"►" : L"▶";
}
#endif
