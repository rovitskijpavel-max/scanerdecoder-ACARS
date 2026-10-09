#include "fileio.h"

#ifdef _WIN32
#include <windows.h>

FILE *fopen_utf8(const char *path, const char *mode) {
    wchar_t wpath[1200], wmode[16];
    /* MB_ERR_INVALID_CHARS: строка, которая не является корректным UTF-8 (ANSI-байты кириллицы почти
     * никогда ей не бывают), не переводится - тогда путь пришёл не из окна, и открываем его по-старому */
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, 1200);
    int m = MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 16);
    if (n > 0 && m > 0)
        return _wfopen(wpath, wmode);
    return fopen(path, mode);
}
#else
FILE *fopen_utf8(const char *path, const char *mode) {
    return fopen(path, mode); /* на Linux/macOS пути и так в UTF-8 */
}
#endif
