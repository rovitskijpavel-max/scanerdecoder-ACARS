/*
 * Встроенная карта с самолётами (Windows).
 *
 * Подложка - обычные тайлы OpenStreetMap (256x256 PNG), которые
 * скачиваются в фоновом потоке и кэшируются на диске в папке
 * acars_tiles\<масштаб>\<x>\<y>.png (рядом с программой): один раз
 * показанная область потом открывается мгновенно и без интернета.
 * Если интернета нет - вместо подложки рисуется координатная сетка,
 * самолёты и следы показываются всё равно.
 *
 * PNG декодируется системной GDI+ (gdiplus.dll), скачивание - WinINet
 * (wininet.dll); обе библиотеки подгружаются на лету через LoadLibrary,
 * поэтому в ключах линковки ничего добавлять не нужно, а если какой-то
 * из них нет - карта просто работает без подложки.
 *
 * Правила использования тайлов OSM (tile usage policy) соблюдены:
 * осмысленный User-Agent, один поток загрузки (максимум одно
 * соединение), локальный кэш на диске, подпись об авторстве в углу
 * карты. Это личный приёмник - нагрузка на серверы OSM ничтожна.
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
/* Windows Imaging Component нужен, чтобы открывать WebP/AVIF/HEIC (GDI+ их не знает). Заголовок есть не
 * во всех сборках MinGW - тогда эта часть просто выключается, а остальная карта собирается как обычно. */
#if defined(__has_include) && !defined(MV_NO_WIC) /* -DMV_NO_WIC отключает эту часть, если заголовок не подошёл */
#if __has_include(<wincodec.h>)
#include <wincodec.h>
#define MV_HAVE_WIC 1
#endif
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include <stddef.h>
#include <ctype.h>
#include <wctype.h>
#include <wchar.h>
#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501 /* ECM_FIRST + 1; нужен comctl32 v6, иначе игнорируется */
#endif
#include "map_view.h"
#include "units.h"
#include "xpcompat.h"
#include "acars_report.h"
#include "labels.h"
#include "airport_db.h"

#ifndef GET_WHEEL_DELTA_WPARAM
#define GET_WHEEL_DELTA_WPARAM(wp) ((short)HIWORD(wp))
#endif
#define MV_GET_X(lp) ((int)(short)LOWORD(lp))
#define MV_GET_Y(lp) ((int)(short)HIWORD(lp))

/* ---------------------------------------------------------------- */
/* Константы                                                        */
/* ---------------------------------------------------------------- */
#define WM_APP_TILE        (WM_APP + 21)
#define IDC_MAP_FIT        201
#define IDC_MAP_ZIN        202
#define IDC_MAP_ZOUT       203
#define IDC_MAP_TRAIL      204
#define IDC_MAP_WEB        205
#define IDC_MAP_STYLE      206
#define IDC_MAP_SAS        207
#define IDC_MAP_OVR        208
#define IDC_MAP_SEARCH     209
#define MAP_TOOLBAR_H      40
#define MAP_PANEL_W_DEFAULT 340   /* ширина панели справа по умолчанию */
#define MAP_PANEL_W_MIN    240    /* уже - не помещается текст */
#define MAP_MAP_W_MIN      320    /* карта не должна пропасть совсем */
#define SPLIT_GRAB         5      /* полуширина зоны захвата разделителя, пиксели */
#define MAP_MIN_ZOOM       2
#define MAP_MAX_ZOOM       15
#define TILE_SIZE          256
#define TILE_CACHE_N       360   /* кадр на большом окне - 70-100 тайлов, плюс запас по краям и тайлы соседних масштабов */
#define MAP_MAX_AIRCRAFT   64
#define TIMER_REFRESH      7001
#define TIMER_REPLAY       7002
#define ROW_H              40

#define CLR_BG        RGB(22, 26, 33)
#define CLR_MAPBG     RGB(34, 40, 48)
#define CLR_TILE_PH   RGB(44, 52, 62)
#define CLR_TILE_LINE RGB(60, 70, 82)
#define CLR_PANEL     RGB(18, 22, 28)
#define CLR_TEXT      RGB(210, 220, 215)
#define CLR_DIM       RGB(140, 150, 150)
#define CLR_AMBER     RGB(255, 196, 0)
#define CLR_AMBER_SEL RGB(255, 240, 120)
#define CLR_PLANE_EDGE RGB(40, 30, 0)
#define CLR_TRAIL     RGB(255, 150, 30)
#define CLR_RING      RGB(80, 200, 255)

/* ---------------------------------------------------------------- */
/* Состояние окна                                                   */
/* ---------------------------------------------------------------- */
static HWND g_hwndMap;
static HWND g_btnFit, g_btnZin, g_btnZout, g_chkTrail, g_btnWeb;
static HFONT g_font, g_fontBold, g_fontSmall;
static HBRUSH g_brToolbar;
static MapProviderFn g_provider;

static MapAircraft g_ac[MAP_MAX_AIRCRAFT];
static int g_nac;
static char g_sel_reg[24];

static double g_clat = 50.0, g_clon = 30.0;
static int g_zoom = 4;
static int g_show_trails = 1;

/* Ширина панели справа - меняется перетаскиванием разделителя между картой и панелью. */
static int g_panel_w = MAP_PANEL_W_DEFAULT;
#define MAP_PANEL_W g_panel_w
static int g_split_drag; /* 1, пока разделитель тащат мышью */

/* Допустимая ширина панели для окна шириной client_w. */
static int clamp_panel_w(int w, int client_w) {
    int hi = client_w - MAP_MAP_W_MIN;
    if (hi < MAP_PANEL_W_MIN)
        hi = MAP_PANEL_W_MIN;
    if (w > hi) w = hi;
    if (w < MAP_PANEL_W_MIN) w = MAP_PANEL_W_MIN;
    return w;
}

static int g_dragging;
static int g_drag_x, g_drag_y;
static double g_drag_wx, g_drag_wy;

/* Состояние сети/декодера - только для понятной подписи на карте. */
static volatile LONG g_net_status; /* 0 - всё нормально/неизвестно, 1 - нет wininet.dll, 2 - ошибка скачивания */
static int g_gdip_failed;

/* ---------------------------------------------------------------- */
/* Стили подложки                                                   */
/* ---------------------------------------------------------------- */
typedef struct {
    const WCHAR *id;          /* стабильное имя для acars_settings.ini (не зависит от порядка в списке) */
    const WCHAR *name;        /* как показано в выпадающем списке */
    const WCHAR *dir;         /* подпапка кэша онлайн-стиля: acars_tiles\<dir>\z\x\y.png */
    const WCHAR *url_fmt;     /* формат адреса для wsprintfW (z, x, y); NULL у локальных */
    const WCHAR *attribution; /* подпись об авторстве - обязательна по лицензии */
    int local;                /* 1 - тайлы читаются из готового кэша SAS.Planet, ничего не скачивается */
    int proj;                 /* GEO_PROJ_SPHERICAL (EPSG:3857) или GEO_PROJ_ELLIPSOID (EPSG:3395) */
    int has_overlay;          /* 1 - у стиля вторая папка: прозрачный слой (подписи, дороги) поверх основы */
} MapStyle;

/* Проекция подложки обязана совпадать с проекцией тайлов: если взять тайлы
 * Яндекса (эллипсоидальный Меркатор, EPSG:3395) и считать по формулам
 * обычного Web Mercator, объекты "съедут" по широте примерно на 20 км на
 * широте Нижнего Новгорода - поэтому проекция хранится в самом стиле, и вся
 * математика карты (положение самолётов, сдвиг, масштаб) считается в ней. */
static const MapStyle kStyles[] = {
    {L"osm", L"OpenStreetMap (стандартная, из интернета)", L"osm",
     L"https://tile.openstreetmap.org/%d/%d/%d.png", L"\u00A9 OpenStreetMap contributors", 0,
     GEO_PROJ_SPHERICAL, 0},
    {L"sas_yandex", L"Кэш SAS.Planet: проекция Яндекса (EPSG:3395)", L"", NULL,
     L"Тайлы из локального кэша SAS.Planet - права принадлежат владельцу источника", 1,
     GEO_PROJ_ELLIPSOID, 0},
    {L"sas_webmerc", L"Кэш SAS.Planet: Web Mercator (Google/OSM/Bing)", L"", NULL,
     L"Тайлы из локального кэша SAS.Planet - права принадлежат владельцу источника", 1,
     GEO_PROJ_SPHERICAL, 0},
    {L"sas_google_hybrid", L"Кэш SAS.Planet: Google гибрид (спутник + подписи)", L"", NULL,
     L"Тайлы из локального кэша SAS.Planet - права принадлежат владельцу источника", 1,
     GEO_PROJ_SPHERICAL, 1},
};
#define N_STYLES ((int)(sizeof(kStyles) / sizeof(kStyles[0])))

static int g_style;       /* индекс текущего стиля в kStyles */

/* Математика карты в проекции ТЕКУЩЕГО стиля (см. комментарий у kStyles). */
#define MV_WX(lon, z)  geo_world_xp((lon), (z), kStyles[g_style].proj)
#define MV_WY(lat, z)  geo_world_yp((lat), (z), kStyles[g_style].proj)
#define MV_LON(x, z)   geo_lon_from_xp((x), (z), kStyles[g_style].proj)
#define MV_LAT(y, z)   geo_lat_from_yp((y), (z), kStyles[g_style].proj)

/* Локальный кэш SAS.Planet: папка карты и найденный в ней диапазон масштабов
 * (-1 - не определён) отдельно для каждого стиля с local=1. */
static WCHAR g_localdir[N_STYLES][MAX_PATH];   /* основа (у "гибрида" - спутник) */
static WCHAR g_overlaydir[N_STYLES][MAX_PATH]; /* прозрачный слой поверх основы (только has_overlay) */
static int g_zmin_s[N_STYLES], g_zmax_s[N_STYLES];
static HWND g_btnSas, g_btnOvr;
static HWND g_cmbStyle;

/* ---------------------------------------------------------------- */
/* Вспомогательное                                                  */
/* ---------------------------------------------------------------- */
static void u8_to_w(const char *u8, WCHAR *out, int cap) {
    out[0] = L'\0';
    if (MultiByteToWideChar(CP_UTF8, 0, u8, -1, out, cap) == 0)
        out[0] = L'\0';
    out[cap - 1] = L'\0';
}

static void put_text(HDC dc, int x, int y, const char *u8) {
    WCHAR w[320];
    u8_to_w(u8, w, 320);
    TextOutW(dc, x, y, w, (int)wcslen(w));
}

static void put_text_rect(HDC dc, RECT *r, const char *u8, UINT flags) {
    WCHAR w[480];
    u8_to_w(u8, w, 480);
    DrawTextW(dc, w, -1, r, flags);
}

static int find_sel(void) {
    for (int i = 0; i < g_nac; i++)
        if (strcmp(g_ac[i].reg, g_sel_reg) == 0)
            return i;
    return -1;
}

/* --- поиск борта на карте: по регистрации, рейсу, аэропортам, типу, компании, Mode S --- */
static HWND g_edSearch;
static char g_search[64];   /* запрос в UTF-8 в нижнем регистре, без пробелов по краям; "" - поиска нет */
static char g_search_n[64]; /* он же без '.', '-' и пробелов - для сравнения с регистрацией и рейсом */
static int g_search_cnt;    /* сколько бортов подходит */
static WNDPROC g_origSearchProc;

static void norm_reg(char *dst, size_t cap, const char *src) {
    size_t j = 0;
    for (; *src && j + 1 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '.' || c == '-' || c == ' ')
            continue;
        dst[j++] = (char)((c < 128) ? tolower(c) : c);
    }
    dst[j] = '\0';
}

static void lower_ascii(char *dst, size_t cap, const char *src) {
    size_t j = 0;
    for (; *src && j + 1 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        dst[j++] = (char)((c < 128) ? tolower(c) : c);
    }
    dst[j] = '\0';
}

static int ac_matches_search(const MapAircraft *a) {
    if (!g_search[0])
        return 0;
    char t[400];
    if (g_search_n[0]) {
        norm_reg(t, sizeof t, a->reg);
        if (strstr(t, g_search_n)) return 1;
        norm_reg(t, sizeof t, a->flight);
        if (a->flight[0] && strstr(t, g_search_n)) return 1;
    }
    const char *f[] = {a->dep, a->dest, a->hex, a->type, a->model, a->operator_name, a->info};
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) {
        if (!f[i][0]) continue;
        lower_ascii(t, sizeof t, f[i]);
        if (strstr(t, g_search)) return 1;
    }
    return 0;
}

static double wrap_lon(double lon) {
    while (lon > 180.0) lon -= 360.0;
    while (lon < -180.0) lon += 360.0;
    return lon;
}

static double clamp_lat_view(double lat) {
    if (lat > 84.0) return 84.0;
    if (lat < -84.0) return -84.0;
    return lat;
}

/* ---------------------------------------------------------------- */
/* GDI+ (подгружается на лету) - только декодирование PNG тайлов    */
/* ---------------------------------------------------------------- */
typedef struct {
    UINT32 GdiplusVersion;
    void *DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} MvGdipStartupInput;

typedef int (WINAPI *PFN_GdiplusStartup)(ULONG_PTR *, const MvGdipStartupInput *, void *);
typedef int (WINAPI *PFN_GdipCreateBitmapFromFile)(const WCHAR *, void **);
typedef int (WINAPI *PFN_GdipCreateHBITMAPFromBitmap)(void *, HBITMAP *, DWORD);
typedef int (WINAPI *PFN_GdipDisposeImage)(void *);

/* Для наложения прозрачного слоя: читаем пиксели обоих тайлов и смешиваем сами. */
typedef struct { int X, Y, Width, Height; } MvGpRect;
typedef struct {
    UINT Width;
    UINT Height;
    int Stride;
    int PixelFormat;
    void *Scan0;
    UINT_PTR Reserved;
} MvBitmapData; /* раскладка совпадает с BitmapData из GDI+ */
typedef int (WINAPI *PFN_GdipBitmapLockBits)(void *, const MvGpRect *, UINT, int, MvBitmapData *);
typedef int (WINAPI *PFN_GdipBitmapUnlockBits)(void *, MvBitmapData *);
typedef int (WINAPI *PFN_GdipGetImageSize)(void *, UINT *);
typedef int (WINAPI *PFN_GdipCreateBitmapFromStream)(void *, void **); /* для фото из памяти */

static int g_gdip_state; /* 0 - ещё не пробовали, 1 - готова, -1 - недоступна */
static ULONG_PTR g_gdip_token;
static PFN_GdipCreateBitmapFromFile pGdipCreateBitmapFromFile;
static PFN_GdipCreateHBITMAPFromBitmap pGdipCreateHBITMAPFromBitmap;
static PFN_GdipDisposeImage pGdipDisposeImage;
static PFN_GdipBitmapLockBits pGdipBitmapLockBits;
static PFN_GdipBitmapUnlockBits pGdipBitmapUnlockBits;
static PFN_GdipGetImageSize pGdipGetImageWidth, pGdipGetImageHeight;
static PFN_GdipCreateBitmapFromStream pGdipCreateBitmapFromStream;
static int g_gdip_lock_ok; /* 1, если доступно чтение пикселей (нужно только для слоя-наложения) */

static int gdip_init(void) {
    if (g_gdip_state != 0)
        return g_gdip_state > 0;
    g_gdip_state = -1;
    HMODULE h = LoadLibraryW(L"gdiplus.dll");
    if (!h)
        return 0;
    PFN_GdiplusStartup start = (PFN_GdiplusStartup)(void *)GetProcAddress(h, "GdiplusStartup");
    pGdipCreateBitmapFromFile =
        (PFN_GdipCreateBitmapFromFile)(void *)GetProcAddress(h, "GdipCreateBitmapFromFile");
    pGdipCreateHBITMAPFromBitmap =
        (PFN_GdipCreateHBITMAPFromBitmap)(void *)GetProcAddress(h, "GdipCreateHBITMAPFromBitmap");
    pGdipDisposeImage = (PFN_GdipDisposeImage)(void *)GetProcAddress(h, "GdipDisposeImage");
    if (!start || !pGdipCreateBitmapFromFile || !pGdipCreateHBITMAPFromBitmap || !pGdipDisposeImage)
        return 0;
    pGdipBitmapLockBits = (PFN_GdipBitmapLockBits)(void *)GetProcAddress(h, "GdipBitmapLockBits");
    pGdipBitmapUnlockBits =
        (PFN_GdipBitmapUnlockBits)(void *)GetProcAddress(h, "GdipBitmapUnlockBits");
    pGdipGetImageWidth = (PFN_GdipGetImageSize)(void *)GetProcAddress(h, "GdipGetImageWidth");
    pGdipGetImageHeight = (PFN_GdipGetImageSize)(void *)GetProcAddress(h, "GdipGetImageHeight");
    pGdipCreateBitmapFromStream =
        (PFN_GdipCreateBitmapFromStream)(void *)GetProcAddress(h, "GdipCreateBitmapFromStream");
    g_gdip_lock_ok = pGdipBitmapLockBits && pGdipBitmapUnlockBits && pGdipGetImageWidth &&
                     pGdipGetImageHeight;
    MvGdipStartupInput in;
    memset(&in, 0, sizeof(in));
    in.GdiplusVersion = 1;
    if (start(&g_gdip_token, &in, NULL) != 0)
        return 0;
    g_gdip_state = 1;
    return 1;
}

static HBITMAP load_png_as_hbitmap(const WCHAR *path) {
    if (!gdip_init())
        return NULL;
    void *bmp = NULL;
    if (pGdipCreateBitmapFromFile(path, &bmp) != 0 || !bmp)
        return NULL;
    HBITMAP hb = NULL;
    int st = pGdipCreateHBITMAPFromBitmap(bmp, &hb, 0);
    pGdipDisposeImage(bmp);
    return st == 0 ? hb : NULL;
}

/* ---------------------------------------------------------------- */
/* Дисковый кэш и очередь загрузки тайлов                           */
/* ---------------------------------------------------------------- */
static WCHAR g_cache_root[MAX_PATH]; /* <рабочая папка>\acars_tiles */

typedef struct { int st, z, x, y; } TileReq;
#define QCAP 64
static TileReq g_q[QCAP];
static int g_qn;
static CRITICAL_SECTION g_qcs;
static HANDLE g_qevt;
static HANDLE g_worker;
static volatile LONG g_quit;

static void tile_file(int st, int z, int x, int y, WCHAR *out) {
    wsprintfW(out, L"%s\\%s\\%d\\%d\\%d.png", g_cache_root, kStyles[st].dir, z, x, y);
}

static void ensure_tile_dirs(int st, int z, int x) {
    WCHAR p[MAX_PATH + 48];
    CreateDirectoryW(g_cache_root, NULL);
    wsprintfW(p, L"%s\\%s", g_cache_root, kStyles[st].dir);
    CreateDirectoryW(p, NULL);
    wsprintfW(p, L"%s\\%s\\%d", g_cache_root, kStyles[st].dir, z);
    CreateDirectoryW(p, NULL);
    wsprintfW(p, L"%s\\%s\\%d\\%d", g_cache_root, kStyles[st].dir, z, x);
    CreateDirectoryW(p, NULL);
}

static void queue_request(int st, int z, int x, int y) {
    EnterCriticalSection(&g_qcs);
    for (int i = 0; i < g_qn; i++) {
        if (g_q[i].st == st && g_q[i].z == z && g_q[i].x == x && g_q[i].y == y) {
            LeaveCriticalSection(&g_qcs);
            return;
        }
    }
    if (g_qn >= QCAP) { /* очередь полна - выбрасываем самый старый запрос */
        memmove(&g_q[0], &g_q[1], (QCAP - 1) * sizeof(TileReq));
        g_qn = QCAP - 1;
    }
    g_q[g_qn].st = st;
    g_q[g_qn].z = z;
    g_q[g_qn].x = x;
    g_q[g_qn].y = y;
    g_qn++;
    LeaveCriticalSection(&g_qcs);
    SetEvent(g_qevt);
}

static int queue_pop(TileReq *out) {
    int ok = 0;
    EnterCriticalSection(&g_qcs);
    if (g_qn > 0) { /* с конца: самый свежий запрос важнее - пользователь уже мог уехать дальше */
        *out = g_q[g_qn - 1];
        g_qn--;
        ok = 1;
    }
    LeaveCriticalSection(&g_qcs);
    return ok;
}

typedef LPVOID MvInet;
typedef MvInet (WINAPI *PFN_InternetOpenW)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
typedef MvInet (WINAPI *PFN_InternetOpenUrlW)(MvInet, LPCWSTR, LPCWSTR, DWORD, DWORD, DWORD_PTR);
typedef BOOL (WINAPI *PFN_InternetReadFile)(MvInet, LPVOID, DWORD, LPDWORD);
typedef BOOL (WINAPI *PFN_InternetCloseHandle)(MvInet);
typedef BOOL (WINAPI *PFN_InternetSetOptionW)(MvInet, DWORD, LPVOID, DWORD);

#define MV_INTERNET_FLAG_RELOAD         0x80000000UL
#define MV_INTERNET_FLAG_NO_CACHE_WRITE 0x04000000UL
#define MV_INTERNET_FLAG_NO_UI          0x00000200UL
#define MV_INTERNET_FLAG_SECURE         0x00800000UL
#define MV_INTERNET_OPTION_CONNECT_TIMEOUT 2
#define MV_INTERNET_OPTION_SEND_TIMEOUT    5
#define MV_INTERNET_OPTION_RECEIVE_TIMEOUT 6

static PFN_InternetOpenUrlW pInternetOpenUrlW;
static PFN_InternetReadFile pInternetReadFile;
static PFN_InternetCloseHandle pInternetCloseHandle;

/* Скачивает один тайл в кэш. Возвращает 1 при успехе. */
static int download_tile(MvInet session, int st, int z, int x, int y) {
    WCHAR url[260];
    wsprintfW(url, kStyles[st].url_fmt, z, x, y);

    MvInet u = pInternetOpenUrlW(session, url, NULL, 0,
                                 MV_INTERNET_FLAG_RELOAD | MV_INTERNET_FLAG_NO_CACHE_WRITE |
                                     MV_INTERNET_FLAG_NO_UI | MV_INTERNET_FLAG_SECURE,
                                 0);
    if (!u)
        return 0;

    const DWORD cap = 262144; /* тайл OSM - десятки КБ, 256 КБ с большим запасом */
    BYTE *buf = (BYTE *)malloc(cap);
    DWORD total = 0;
    if (buf) {
        for (;;) {
            DWORD got = 0;
            if (!pInternetReadFile(u, buf + total, cap - total, &got) || got == 0)
                break;
            total += got;
            if (total >= cap)
                break;
        }
    }
    pInternetCloseHandle(u);

    /* Ответ должен быть именно PNG: страница с ошибкой 404/403 тоже
     * "скачивается успешно", но сохранять её как тайл нельзя. */
    int ok = 0;
    if (buf && total > 100 && buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G') {
        WCHAR final_path[MAX_PATH + 32], tmp_path[MAX_PATH + 40];
        tile_file(st, z, x, y, final_path);
        wsprintfW(tmp_path, L"%s.tmp", final_path);
        ensure_tile_dirs(st, z, x);
        HANDLE f = CreateFileW(tmp_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD wr = 0;
            BOOL w_ok = WriteFile(f, buf, total, &wr, NULL);
            CloseHandle(f);
            if (w_ok && wr == total && MoveFileExW(tmp_path, final_path, MOVEFILE_REPLACE_EXISTING))
                ok = 1;
            else
                DeleteFileW(tmp_path);
        }
    }
    free(buf);
    return ok;
}

static DWORD WINAPI tile_worker(LPVOID param) {
    (void)param;
    HMODULE hw = LoadLibraryW(L"wininet.dll");
    PFN_InternetOpenW pOpen = NULL;
    PFN_InternetSetOptionW pSetOpt = NULL;
    if (hw) {
        pOpen = (PFN_InternetOpenW)(void *)GetProcAddress(hw, "InternetOpenW");
        pInternetOpenUrlW = (PFN_InternetOpenUrlW)(void *)GetProcAddress(hw, "InternetOpenUrlW");
        pInternetReadFile = (PFN_InternetReadFile)(void *)GetProcAddress(hw, "InternetReadFile");
        pInternetCloseHandle =
            (PFN_InternetCloseHandle)(void *)GetProcAddress(hw, "InternetCloseHandle");
        pSetOpt = (PFN_InternetSetOptionW)(void *)GetProcAddress(hw, "InternetSetOptionW");
    }
    MvInet session = NULL;
    if (pOpen && pInternetOpenUrlW && pInternetReadFile && pInternetCloseHandle) {
        session = pOpen(L"ACARSDecoder/1.0 (personal ACARS receiver; local tile cache)",
                        0 /* INTERNET_OPEN_TYPE_PRECONFIG */, NULL, NULL, 0);
        if (session && pSetOpt) {
            DWORD ms = 8000;
            pSetOpt(session, MV_INTERNET_OPTION_CONNECT_TIMEOUT, &ms, sizeof(ms));
            pSetOpt(session, MV_INTERNET_OPTION_SEND_TIMEOUT, &ms, sizeof(ms));
            pSetOpt(session, MV_INTERNET_OPTION_RECEIVE_TIMEOUT, &ms, sizeof(ms));
        }
    }
    if (!session)
        InterlockedExchange(&g_net_status, 1);

    while (!g_quit) {
        WaitForSingleObject(g_qevt, INFINITE);
        if (g_quit)
            break;
        TileReq r;
        while (!g_quit && queue_pop(&r)) {
            int ok = session ? download_tile(session, r.st, r.z, r.x, r.y) : 0;
            if (session)
                InterlockedExchange(&g_net_status, ok ? 0 : 2);
            if (g_hwndMap)
                PostMessageW(g_hwndMap, WM_APP_TILE,
                             (WPARAM)((DWORD)r.x | ((DWORD)r.z << 16) | ((DWORD)(ok ? 1 : 0) << 24) |
                                        ((DWORD)r.st << 25)),
                             (LPARAM)r.y);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* Кэш тайлов в памяти (уже декодированные HBITMAP)                  */
/* ---------------------------------------------------------------- */
enum { TS_EMPTY = 0, TS_READY = 1, TS_FAILED = 2, TS_PENDING = 3 };

typedef struct {
    int used;
    int st, z, x, y;
    int state;
    unsigned use_stamp;
    DWORD fail_time;
    DWORD retry_ms; /* через сколько пробовать снова после неудачи; 0 - стандартные 30 с */
    HBITMAP bmp;
    HBITMAP fbmp;   /* сглаженная подстановка вместо отсутствующего тайла (из тайла другого масштаба) */
    int fb_k;       /* из предка на сколько уровней выше она построена */
} TileSlot;

static TileSlot g_tiles[TILE_CACHE_N];
static unsigned g_use_counter;
static HBITMAP g_fb_src_h; /* чьи пиксели лежат в g_fb_pix (чтобы не читать их заново для соседних тайлов) */
/* 1 - показывать подробную служебную информацию о кэше (клавиша I); по умолчанию карта чистая */
static int g_show_info;
static int g_sel_pt = -1;   /* выбранная точка следа у выбранного борта (0 - самая старая), -1 - последняя */
static int g_trk_first, g_trk_rows, g_trk_top; /* строки таблицы следа в панели (для клика) */
#define TRK_ROW_H 18
static int g_silver_count; /* сколько серых тайлов-заглушек встретилось в кэше SAS.Planet (для подписи) */
/* Итоги последней отрисовки кадра (для подписи в углу): сколько тайлов нужно и сколько нашлось, и
 * какого именно не хватает первым - чтобы по скриншоту было видно, что искать в папке кэша. */
static int g_view_total, g_view_have, g_view_pending, g_view_miss_have, g_view_miss_z, g_view_miss_x, g_view_miss_y;
static int g_view_miss_n, g_view_miss_x0, g_view_miss_x1, g_view_miss_y0, g_view_miss_y1; /* число и габариты недостающих тайлов кадра */

static TileSlot *tile_find(int st, int z, int x, int y) {
    for (int i = 0; i < TILE_CACHE_N; i++)
        if (g_tiles[i].used && g_tiles[i].st == st && g_tiles[i].z == z && g_tiles[i].x == x &&
            g_tiles[i].y == y)
            return &g_tiles[i];
    return NULL;
}

static TileSlot *tile_alloc(int st, int z, int x, int y) {
    int pick = -1;
    for (int i = 0; i < TILE_CACHE_N; i++) {
        if (!g_tiles[i].used) {
            pick = i;
            break;
        }
    }
    if (pick < 0) { /* вытесняем самый давно использованный, не ожидающий загрузки */
        unsigned best = 0xFFFFFFFFu;
        for (int i = 0; i < TILE_CACHE_N; i++) {
            if (g_tiles[i].state != TS_PENDING && g_tiles[i].use_stamp <= best) {
                best = g_tiles[i].use_stamp;
                pick = i;
            }
        }
        if (pick < 0)
            pick = 0;
    }
    TileSlot *s = &g_tiles[pick];
    if (s->used && s->bmp) {
        if (s->bmp == g_fb_src_h)
            g_fb_src_h = NULL;
        DeleteObject(s->bmp);
    }
    if (s->used && s->fbmp)
        DeleteObject(s->fbmp);
    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->st = st;
    s->z = z;
    s->x = x;
    s->y = y;
    s->state = TS_EMPTY;
    return s;
}

/* ---- чтение тайла из кэша SAS.Planet ---- */
static void sas_tile_base(const WCHAR *dir, int z, int x, int y, WCHAR *out) {
    /* <папка карты>\z[Z+1]\[X/1024]\x[X]\[Y/1024]\y[Y] - у SAS.Planet номер
     * масштаба в имени папки на единицу больше обычного (z=0 лежит в z1) */
    wsprintfW(out, L"%s\\z%d\\%d\\x%d\\%d\\y%d", dir, z + 1, x / 1024, x, y / 1024, y);
}

/* Копирует пиксели BGRA (порядок байт B,G,R,A) из src в dst, делая их непрозрачными. */
static void copy_bgra_opaque(BYTE *dst, int dst_stride, const BYTE *src, int src_stride, int w, int h) {
    for (int y = 0; y < h; y++) {
        const BYTE *s = src + (ptrdiff_t)y * src_stride;
        BYTE *d = dst + (ptrdiff_t)y * dst_stride;
        for (int x = 0; x < w; x++, s += 4, d += 4) {
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = 255;
        }
    }
}

/* Накладывает слой src (BGRA, ПРЯМАЯ альфа - именно так отдаёт пиксели GDI+) на
 * непрозрачное изображение dst. Полностью прозрачный пиксель слоя оставляет основу
 * как есть, полностью непрозрачный - заменяет; между ними - взвешенная сумма. */
static void blend_bgra_over(BYTE *dst, int dst_stride, const BYTE *src, int src_stride, int w, int h) {
    for (int y = 0; y < h; y++) {
        const BYTE *s = src + (ptrdiff_t)y * src_stride;
        BYTE *d = dst + (ptrdiff_t)y * dst_stride;
        for (int x = 0; x < w; x++, s += 4, d += 4) {
            unsigned a = s[3];
            if (a == 255) {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
            } else if (a != 0) {
                d[0] = (BYTE)((s[0] * a + d[0] * (255 - a) + 127) / 255);
                d[1] = (BYTE)((s[1] * a + d[1] * (255 - a) + 127) / 255);
                d[2] = (BYTE)((s[2] * a + d[2] * (255 - a) + 127) / 255);
            }
            d[3] = 255;
        }
    }
}

/* Тайл целиком залит одним серым (192,192,192) - это "clSilver" из Delphi, на котором написан
 * SAS.Planet, обычным цветом "пустого места". Настоящий снимок или схема так не выглядят:
 * для этого ВСЕ 65536 пикселей должны быть серыми с точностью до +-4 (запас на JPEG). Такой
 * тайл - заглушка "данных нет", а не картинка; рисовать его серым квадратом бессмысленно. */
static int bgra_is_silver(const BYTE *px, int stride, int w, int h) {
    for (int y = 0; y < h; y++) {
        const BYTE *p = px + (ptrdiff_t)y * stride;
        for (int x = 0; x < w; x++, p += 4) {
            int b = p[0] - 192, g = p[1] - 192, r = p[2] - 192;
            if (b < -4 || b > 4 || g < -4 || g > 4 || r < -4 || r > 4)
                return 0; /* у настоящего тайла выход происходит на первых же пикселях */
        }
    }
    return 1;
}

/* То же для готового HBITMAP (путь без наложения слоя). Пиксели берём через GetDIBits - она
 * работает с любым растром, без догадок о его внутреннем устройстве. 0 при любой неудаче. */
static int hbitmap_is_silver(HBITMAP hb) {
    static BYTE buf[TILE_SIZE * TILE_SIZE * 4];
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = TILE_SIZE;
    bmi.bmiHeader.biHeight = -TILE_SIZE;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(NULL);
    if (!dc)
        return 0;
    int lines = GetDIBits(dc, hb, 0, TILE_SIZE, buf, &bmi, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);
    if (lines != TILE_SIZE)
        return 0;
    return bgra_is_silver(buf, TILE_SIZE * 4, TILE_SIZE, TILE_SIZE);
}

/* Блокирует пиксели тайла 256x256 на чтение в формате ARGB (GDI+ сама приводит к нему
 * любой исходный формат - jpg, png с прозрачностью и т.д.). 1 - успех. */
static int lock_bgra_256(void *bmp, MvBitmapData *bd) {
    UINT w = 0, h = 0;
    if (pGdipGetImageWidth(bmp, &w) != 0 || pGdipGetImageHeight(bmp, &h) != 0 ||
        w != TILE_SIZE || h != TILE_SIZE)
        return 0;
    MvGpRect r = {0, 0, TILE_SIZE, TILE_SIZE};
    memset(bd, 0, sizeof(*bd));
    return pGdipBitmapLockBits(bmp, &r, 1 /* ImageLockModeRead */, 0x0026200A /* 32bppARGB */, bd) == 0;
}

/* Собирает один тайл из основы (файл base_path, может отсутствовать) и прозрачного
 * слоя (ovl_path, может отсутствовать). Там, где нет основы, остаётся цвет заглушки. */
static HBITMAP compose_sas_tile(const WCHAR *base_path, const WCHAR *ovl_path, int *base_silver) {
    void *bb = NULL, *ob = NULL;
    if (base_path && pGdipCreateBitmapFromFile(base_path, &bb) != 0)
        bb = NULL;
    if (ovl_path && pGdipCreateBitmapFromFile(ovl_path, &ob) != 0)
        ob = NULL;
    if (!bb && !ob)
        return NULL;

    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = TILE_SIZE;
    bmi.bmiHeader.biHeight = -TILE_SIZE; /* минус - строки сверху вниз, как у тайла */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP hb = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    int drew = 0;
    if (hb && bits) {
        BYTE *px = (BYTE *)bits;
        const int stride = TILE_SIZE * 4;
        for (int i = 0; i < TILE_SIZE * TILE_SIZE; i++) {
            px[i * 4 + 0] = GetBValue(CLR_TILE_PH);
            px[i * 4 + 1] = GetGValue(CLR_TILE_PH);
            px[i * 4 + 2] = GetRValue(CLR_TILE_PH);
            px[i * 4 + 3] = 255;
        }
        MvBitmapData bd;
        if (bb && lock_bgra_256(bb, &bd)) {
            if (bgra_is_silver((const BYTE *)bd.Scan0, bd.Stride, TILE_SIZE, TILE_SIZE)) {
                *base_silver = 1; /* серая заглушка вместо снимка - считаем, что основы нет */
            } else {
                copy_bgra_opaque(px, stride, (const BYTE *)bd.Scan0, bd.Stride, TILE_SIZE, TILE_SIZE);
                drew = 1;
            }
            pGdipBitmapUnlockBits(bb, &bd);
        }
        if (ob && lock_bgra_256(ob, &bd)) {
            blend_bgra_over(px, stride, (const BYTE *)bd.Scan0, bd.Stride, TILE_SIZE, TILE_SIZE);
            pGdipBitmapUnlockBits(ob, &bd);
            drew = 1;
        }
    }
    if (bb)
        pGdipDisposeImage(bb);
    if (ob)
        pGdipDisposeImage(ob);
    if (hb && !drew) { /* ни одна картинка не подошла (не 256x256) - пустая заглушка не нужна */
        DeleteObject(hb);
        hb = NULL;
    }
    return hb;
}

/* Ищет файл тайла в папке карты, пробуя расширения по порядку. 1 - найден (путь в out). */
static int find_sas_file(const WCHAR *dir, int z, int x, int y, const WCHAR *const *exts, int n,
                         WCHAR *out) {
    WCHAR base[MAX_PATH + 96];
    sas_tile_base(dir, z, x, y, base);
    for (int i = 0; i < n; i++) {
        wsprintfW(out, L"%s%s", base, exts[i]);
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES)
            return 1;
    }
    return 0;
}

/* Читает тайл из кэша SAS.Planet (и накладывает слой, если задан ovl_dir). Пути приходят параметрами, а не из
 * глобальных переменных: функция вызывается из фонового потока загрузки, пока окно живёт своей жизнью. */
static HBITMAP load_sas_tile_dirs(const WCHAR *base_dir, const WCHAR *ovl_dir, int z, int x, int y, int *empty) {
    *empty = 0;
    if (!gdip_init()) {
        g_gdip_failed = 1;
        return NULL;
    }
    static const WCHAR *const base_exts[] = {L".jpg", L".png", L".gif", L".jpeg"};
    static const WCHAR *const ovl_exts[] = {L".png", L".gif", L".jpg", L".jpeg"};
    WCHAR bp[MAX_PATH + 112], op[MAX_PATH + 112];
    int has_base = find_sas_file(base_dir, z, x, y, base_exts, 4, bp);
    int has_ovl = ovl_dir[0] && g_gdip_lock_ok && find_sas_file(ovl_dir, z, x, y, ovl_exts, 4, op);
    if (has_ovl) {
        int silver = 0;
        HBITMAP hb = compose_sas_tile(has_base ? bp : NULL, op, &silver);
        if (hb)
            return hb;
        if (silver) { /* основа - серая заглушка, а слоя на этом месте нет */
            *empty = 1;
            return NULL;
        }
        /* не вышло смешать - пусть будет хотя бы основа */
    }
    if (has_base) {
        HBITMAP hb = load_png_as_hbitmap(bp); /* GDI+ сама определяет формат (jpg/png/gif) */
        if (hb && hbitmap_is_silver(hb)) {
            DeleteObject(hb);
            *empty = 1;
            return NULL;
        }
        return hb;
    }
    return NULL;
}

/* ---------------------------------------------------------------- */
/* Фоновая загрузка тайлов с диска                                   */
/* ---------------------------------------------------------------- */
/* Раньше тайл читался и декодировался прямо во время перерисовки, в потоке окна: каждая новая полоса
 * карты при сдвиге тормозила кадр. Теперь окно только заказывает тайл (load_enqueue) и рисует то, что
 * уже есть; отдельный поток (tile_loader) читает файлы и присылает готовые битмапы сообщением. */
#define WM_APP_TILEDONE (WM_APP + 23)
#define LCAP 128

typedef struct {
    int st, z, x, y;
    int prio;      /* 0 - тайл виден сейчас, 1 - запас по краям (предзагрузка) */
    int local;     /* 1 - кэш SAS.Planet, 0 - файл из acars_tiles (онлайн-стиль) */
    unsigned gen;  /* поколение кэша: после смены стиля/папки ответы старого поколения выбрасываются */
    WCHAR base[MAX_PATH];
    WCHAR ovl[MAX_PATH];
} LoadReq;

typedef struct {
    int st, z, x, y;
    unsigned gen;
    int status;    /* 1 - готово, 0 - не вышло, 2 - файла нет (онлайн-стиль: надо скачать), 3 - серая заглушка */
    HBITMAP hb;
} TileResult;

static LoadReq g_lq[LCAP];
static int g_lqn;
static CRITICAL_SECTION g_lqcs;
static HANDLE g_lqevt, g_lworker;
static unsigned g_tile_gen; /* меняется только в потоке окна */

/* Какой запрос взять из очереди: с наименьшим prio (видимые раньше запаса); среди равных - самый
 * свежий (с наибольшим номером): пользователь мог уже уехать, важнее то, что он видит сейчас.
 * -1 - очередь пуста. */
static int pick_load_req(const int *prio, int n) {
    int best = -1;
    for (int i = 0; i < n; i++)
        if (best < 0 || prio[i] <= prio[best])
            best = i;
    return best;
}

static TileSlot *tile_find(int st, int z, int x, int y);

/* Заказывает загрузку тайла (поток окна). Запросы чужого масштаба/стиля выбрасываются - при зуме они уже
 * не нужны, а их ячейки возвращаются в "пусто", чтобы тайл можно было заказать снова. */
static void load_enqueue(int st, int z, int x, int y, int prio) {
    LoadReq r;
    r.st = st;
    r.z = z;
    r.x = x;
    r.y = y;
    r.prio = prio;
    r.local = kStyles[st].local;
    r.gen = g_tile_gen;
    r.base[0] = r.ovl[0] = L'\0';
    if (r.local) {
        wcscpy(r.base, g_localdir[st]);
        if (kStyles[st].has_overlay)
            wcscpy(r.ovl, g_overlaydir[st]);
    }

    int dropped[LCAP + 2][4], nd = 0;
    EnterCriticalSection(&g_lqcs);
    for (int i = 0; i < g_lqn; i++) {
        if (g_lq[i].st == st && g_lq[i].z == z && g_lq[i].x == x && g_lq[i].y == y) { /* уже заказан */
            if (prio < g_lq[i].prio)
                g_lq[i].prio = prio;
            LeaveCriticalSection(&g_lqcs);
            return;
        }
    }
    int w = 0;
    for (int i = 0; i < g_lqn; i++) {
        if (g_lq[i].st == st && g_lq[i].z == z) {
            g_lq[w++] = g_lq[i];
        } else if (nd < LCAP) {
            dropped[nd][0] = g_lq[i].st; dropped[nd][1] = g_lq[i].z; dropped[nd][2] = g_lq[i].x; dropped[nd][3] = g_lq[i].y;
            nd++;
        }
    }
    g_lqn = w;
    if (g_lqn >= LCAP) { /* очередь полна - выбрасываем наименее нужный: наибольший prio, из них самый старый */
        int victim = 0;
        for (int i = 1; i < g_lqn; i++)
            if (g_lq[i].prio > g_lq[victim].prio)
                victim = i;
        if (nd < LCAP + 2) {
            dropped[nd][0] = g_lq[victim].st; dropped[nd][1] = g_lq[victim].z;
            dropped[nd][2] = g_lq[victim].x; dropped[nd][3] = g_lq[victim].y;
            nd++;
        }
        memmove(&g_lq[victim], &g_lq[victim + 1], (size_t)(g_lqn - victim - 1) * sizeof(LoadReq));
        g_lqn--;
    }
    g_lq[g_lqn++] = r;
    LeaveCriticalSection(&g_lqcs);
    SetEvent(g_lqevt);

    for (int i = 0; i < nd; i++) {
        TileSlot *d = tile_find(dropped[i][0], dropped[i][1], dropped[i][2], dropped[i][3]);
        if (d && d->state == TS_PENDING)
            d->state = TS_EMPTY;
    }
}

/* Тайл, заказанный про запас, стал видимым - поднимаем его в очереди до самого важного. */
static void load_bump(int st, int z, int x, int y) {
    EnterCriticalSection(&g_lqcs);
    for (int i = 0; i < g_lqn; i++) {
        if (g_lq[i].st == st && g_lq[i].z == z && g_lq[i].x == x && g_lq[i].y == y) {
            g_lq[i].prio = 0;
            break;
        }
    }
    LeaveCriticalSection(&g_lqcs);
}

static int load_pop(LoadReq *out) {
    int ok = 0;
    EnterCriticalSection(&g_lqcs);
    if (g_lqn > 0) {
        int prio[LCAP];
        for (int i = 0; i < g_lqn; i++)
            prio[i] = g_lq[i].prio;
        int k = pick_load_req(prio, g_lqn);
        *out = g_lq[k];
        memmove(&g_lq[k], &g_lq[k + 1], (size_t)(g_lqn - k - 1) * sizeof(LoadReq));
        g_lqn--;
        ok = 1;
    }
    LeaveCriticalSection(&g_lqcs);
    return ok;
}

/* Фоновый поток: читает и декодирует тайлы. Ни к ячейкам, ни к окну не прикасается - только присылает
 * сообщение с готовым результатом. */
static DWORD WINAPI tile_loader(LPVOID param) {
    (void)param;
    for (;;) {
        WaitForSingleObject(g_lqevt, INFINITE);
        LoadReq r;
        while (load_pop(&r)) {
            TileResult *res = (TileResult *)calloc(1, sizeof(TileResult));
            if (!res)
                continue;
            res->st = r.st;
            res->z = r.z;
            res->x = r.x;
            res->y = r.y;
            res->gen = r.gen;
            if (r.local) {
                int empty = 0;
                res->hb = load_sas_tile_dirs(r.base, r.ovl, r.z, r.x, r.y, &empty);
                res->status = res->hb ? 1 : (empty ? 3 : 0);
            } else {
                WCHAR path[MAX_PATH + 32];
                tile_file(r.st, r.z, r.x, r.y, path);
                if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
                    res->status = 2; /* в дисковом кэше нет - окно закажет скачивание */
                } else {
                    res->hb = load_png_as_hbitmap(path);
                    if (res->hb) {
                        res->status = 1;
                    } else {
                        DeleteFileW(path); /* битый файл в кэше - удаляем, скачаем заново */
                        res->status = 2;
                    }
                }
            }
            if (!g_hwndMap || !PostMessageW(g_hwndMap, WM_APP_TILEDONE, 0, (LPARAM)res)) {
                if (res->hb)
                    DeleteObject(res->hb);
                free(res);
            }
        }
    }
    return 0;
}

/* Результат фонового потока пришёл в окно - кладём тайл в свою ячейку (поток окна). */
static void tile_result_apply(TileResult *r) {
    int keep = 0;
    if (r->gen == g_tile_gen) { /* ответ не устарел (стиль/папку не меняли) */
        TileSlot *s = tile_find(r->st, r->z, r->x, r->y);
        if (s) {
            if (r->status == 1 && r->hb) {
                if (s->bmp) {
                    if (s->bmp == g_fb_src_h)
                        g_fb_src_h = NULL;
                    DeleteObject(s->bmp);
                }
                if (s->fbmp) { /* настоящий тайл пришёл - временная подстановка больше не нужна */
                    DeleteObject(s->fbmp);
                    s->fbmp = NULL;
                }
                s->bmp = r->hb;
                s->state = TS_READY;
                keep = 1;
            } else if (r->status == 2) { /* онлайн-стиль, в дисковом кэше нет - скачиваем */
                queue_request(r->st, r->z, r->x, r->y);
                s->state = TS_PENDING;
                s->fail_time = GetTickCount();
            } else {
                int was_empty = (s->retry_ms == 300000);
                s->state = TS_FAILED;
                s->fail_time = GetTickCount();
                /* нет файла - вдруг дозагрузят, проверим снова через 30 с; серая заглушка - сознательная
                 * "пустота" в самом кэше, перечитывать её каждые 30 с незачем */
                s->retry_ms = (r->status == 3) ? 300000 : 30000;
                if (r->status == 3 && !was_empty)
                    g_silver_count++;
            }
        }
    }
    if (!keep && r->hb)
        DeleteObject(r->hb);
    free(r);
}

/* Возвращает готовый HBITMAP тайла или NULL (тогда его надо дорисовать чем-то временным). Сам тайл
 * заказывается у фонового потока; *state - его состояние (TS_READY, TS_PENDING - ещё грузится,
 * TS_FAILED - в кэше нет). prio: 0 - тайл виден, 1 - запас. */
static HBITMAP tile_get(int z, int x, int y, int prio, int *state) {
    TileSlot *s = tile_find(g_style, z, x, y);
    if (!s)
        s = tile_alloc(g_style, z, x, y);
    s->use_stamp = ++g_use_counter;
    DWORD now = GetTickCount();

    if (s->state == TS_FAILED && now - s->fail_time > (s->retry_ms ? s->retry_ms : 30000))
        s->state = TS_EMPTY; /* через 30 секунд пробуем снова */
    if (s->state == TS_PENDING && now - s->fail_time > 20000)
        s->state = TS_EMPTY; /* запрос потерялся (выброшен из очереди) - закажем заново */

    if (s->state == TS_EMPTY) {
        s->state = TS_PENDING;
        s->fail_time = now; /* здесь поле значит "когда заказали" */
        load_enqueue(g_style, z, x, y, prio);
    }
    else if (s->state == TS_PENDING && prio == 0)
        load_bump(g_style, z, x, y);
    if (state)
        *state = s->state;
    return s->state == TS_READY ? s->bmp : NULL;
}

/* Тайл из памяти, если он уже готов; ничего не заказывает. Нужен, чтобы временно показать на месте
 * ещё не загруженного тайла его растянутого "соседа" другого масштаба. */
static HBITMAP tile_peek_bmp(int z, int x, int y) {
    TileSlot *s = tile_find(g_style, z, x, y);
    if (s && s->state == TS_READY && s->bmp) {
        s->use_stamp = ++g_use_counter; /* пока он нужен как подстановка, из памяти его не выбрасываем */
        return s->bmp;
    }
    return NULL;
}

/* Заказывает тайл про запас (за краем кадра), если его ещё нет и не заказан. */
static void tile_prefetch(int z, int x, int y) {
    TileSlot *s = tile_find(g_style, z, x, y);
    if (s) {
        s->use_stamp = ++g_use_counter;
        return;
    }
    s = tile_alloc(g_style, z, x, y);
    s->use_stamp = ++g_use_counter;
    s->state = TS_PENDING;
    s->fail_time = GetTickCount();
    load_enqueue(g_style, z, x, y, 1);
}

/* Параметры подстановки: тайл (x, y) масштаба z можно временно заменить куском тайла k уровней выше
 * (крупнее, ему нужен только участок): его координаты (ax, ay), размер участка и смещение в нём. */
static void ancestor_src(int x, int y, int k, int *ax, int *ay, int *sx, int *sy, int *size) {
    *size = TILE_SIZE >> k;
    *ax = x >> k;
    *ay = y >> k;
    *sx = (x & ((1 << k) - 1)) * *size;
    *sy = (y & ((1 << k) - 1)) * *size;
}

/* Чем заменить тайл, пока он не загрузился. Вместо тёмного квадрата с рамкой (он бросался в глаза и при
 * каждом зуме на секунду "ронял" всю картинку): 1) растянутый кусок тайла более крупного масштаба,
 * 2) четыре тайла более мелкого масштаба, сжатые вдвое, 3) иначе - просто фон. Берутся только готовые
 * тайлы из памяти. fill - цвет фона. */
static void draw_tile_fallback(HDC dc, HDC tdc, int z, int x, int y, int dx, int dy, HBRUSH fill) {
    for (int k = 1; k <= 4 && z - k >= 0; k++) {
        int ax, ay, sx, sy, size;
        ancestor_src(x, y, k, &ax, &ay, &sx, &sy, &size);
        HBITMAP hb = tile_peek_bmp(z - k, ax, ay);
        if (hb) {
            HGDIOBJ old = SelectObject(tdc, hb);
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchBlt(dc, dx, dy, TILE_SIZE, TILE_SIZE, tdc, sx, sy, size, size, SRCCOPY);
            SelectObject(tdc, old);
            return;
        }
    }
    RECT r = {dx, dy, dx + TILE_SIZE, dy + TILE_SIZE};
    FillRect(dc, &r, fill);
    if (z + 1 <= 24) {
        for (int j = 0; j < 2; j++) {
            for (int i = 0; i < 2; i++) {
                HBITMAP hb = tile_peek_bmp(z + 1, 2 * x + i, 2 * y + j);
                if (hb) {
                    HGDIOBJ old = SelectObject(tdc, hb);
                    SetStretchBltMode(dc, HALFTONE);
                    SetBrushOrgEx(dc, 0, 0, NULL);
                    StretchBlt(dc, dx + i * (TILE_SIZE / 2), dy + j * (TILE_SIZE / 2), TILE_SIZE / 2, TILE_SIZE / 2, tdc, 0,
                               0, TILE_SIZE, TILE_SIZE, SRCCOPY);
                    SelectObject(tdc, old);
                }
            }
        }
    }
}

/* ---------------------------------------------------------------- */
/* Настройки карты и переключение стиля                              */
/* ---------------------------------------------------------------- */
static void cwd_file(const WCHAR *name, WCHAR *out) {
    WCHAR cwd[MAX_PATH];
    cwd[0] = L'\0';
    GetCurrentDirectoryW(MAX_PATH, cwd);
    wsprintfW(out, L"%s\\%s", cwd, name);
}

/* ---------------------------------------------------------------- */
/* Кэш SAS.Planet: поиск папки, диапазон масштабов                   */
/* ---------------------------------------------------------------- */
static int digits_only(const WCHAR *s, int a, int b) { /* s[a..b) - непустая строка из цифр */
    if (b <= a)
        return 0;
    for (int i = a; i < b; i++)
        if (s[i] < L'0' || s[i] > L'9')
            return 0;
    return 1;
}

/* "z13" -> 12 (масштаб в SAS.Planet в имени папки на 1 больше), иначе -1. */
static int sas_zoom_from_folder(const WCHAR *name) {
    if (name[0] != L'z' && name[0] != L'Z')
        return -1;
    int n = 0, digits = 0;
    for (const WCHAR *p = name + 1; *p; p++) {
        if (*p < L'0' || *p > L'9' || ++digits > 3)
            return -1;
        n = n * 10 + (*p - L'0');
    }
    return (digits > 0 && n >= 1) ? n - 1 : -1;
}

/* Из пути к любому файлу тайла вида
 *   <папка карты>\z13\18\x19141\9\y9525.jpg
 * выделяет <папка карты>. Проверяет всю структуру (z<цифры>, цифры, x<цифры>,
 * цифры, y<цифры>.<расширение>), чтобы нельзя было случайно указать не тот файл. */
static int sas_map_dir_from_tile(const WCHAR *file, WCHAR *out, int cap) {
    int len = (int)wcslen(file);
    if (len < 12 || len >= cap)
        return 0;
    int sep[5], ns = 0;
    for (int i = len - 1; i >= 0 && ns < 5; i--)
        if (file[i] == L'\\' || file[i] == L'/')
            sep[ns++] = i;
    if (ns < 5 || sep[4] <= 0)
        return 0;

    int f0 = sep[0] + 1; /* y<цифры>.<расширение> */
    if (file[f0] != L'y' && file[f0] != L'Y')
        return 0;
    int dot = -1;
    for (int i = f0 + 1; i < len; i++)
        if (file[i] == L'.')
            dot = i;
    if (dot < 0 || dot + 1 >= len || !digits_only(file, f0 + 1, dot))
        return 0;
    if (!digits_only(file, sep[1] + 1, sep[0])) /* Y/1024 */
        return 0;
    if ((file[sep[2] + 1] != L'x' && file[sep[2] + 1] != L'X') ||
        !digits_only(file, sep[2] + 2, sep[1])) /* x<X> */
        return 0;
    if (!digits_only(file, sep[3] + 1, sep[2])) /* X/1024 */
        return 0;
    if ((file[sep[4] + 1] != L'z' && file[sep[4] + 1] != L'Z') ||
        !digits_only(file, sep[4] + 2, sep[3])) /* z<Z+1> */
        return 0;

    memcpy(out, file, (size_t)sep[4] * sizeof(WCHAR));
    out[sep[4]] = L'\0';
    return 1;
}

/* Определяет, какие масштабы (папки z<N>) есть в папке dir. 1 - найден хотя бы один. */
static int sas_scan_dir(const WCHAR *dir, int *zmin_out, int *zmax_out) {
    WCHAR pat[MAX_PATH + 8];
    wsprintfW(pat, L"%s\\z*", dir);
    WIN32_FIND_DATAW fd;
    int zmin = -1, zmax = -1;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                int z = sas_zoom_from_folder(fd.cFileName);
                if (z >= 0) {
                    if (zmin < 0 || z < zmin) zmin = z;
                    if (z > zmax) zmax = z;
                }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (zmin_out) *zmin_out = zmin;
    if (zmax_out) *zmax_out = zmax;
    return zmax >= 0;
}

static int sas_scan_zooms(int st) {
    int zmin, zmax;
    int ok = sas_scan_dir(g_localdir[st], &zmin, &zmax);
    g_zmin_s[st] = zmin;
    g_zmax_s[st] = zmax;
    return ok;
}

/* Из папки, которую выбрал пользователь, находит папку КАРТЫ (ту, внутри
 * которой лежат z<N>). Прощает типичные промахи:
 *   - выбрана сама папка карты                       -> она;
 *   - выбрана папка масштаба (z9)                    -> её родитель;
 *   - выбрана папка кэша, а в ней ровно одна карта   -> эта карта;
 *   - в выбранной папке несколько карт               -> -1 и список имён в names;
 *   - ничего похожего                                -> 0.
 * Вглубь не ищет (только на один уровень) - чтобы не сканировать диск. */
static int sas_resolve_folder(const WCHAR *picked_in, WCHAR *out, WCHAR *names, int names_cap) {
    WCHAR picked[MAX_PATH];
    int len = (int)wcslen(picked_in);
    if (len <= 0 || len >= MAX_PATH - 12)
        return 0;
    wcscpy(picked, picked_in);
    while (len > 3 && (picked[len - 1] == L'\\' || picked[len - 1] == L'/'))
        picked[--len] = L'\0'; /* "C:\карта\" -> "C:\карта"; корень диска "C:\" оставляем */
    names[0] = L'\0';

    if (sas_scan_dir(picked, NULL, NULL)) {
        wcscpy(out, picked);
        return 1;
    }

    int sep = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (picked[i] == L'\\' || picked[i] == L'/') {
            sep = i;
            break;
        }
    }
    if (sep > 0 && sas_zoom_from_folder(picked + sep + 1) >= 0) {
        WCHAR parent[MAX_PATH];
        memcpy(parent, picked, (size_t)sep * sizeof(WCHAR));
        parent[sep] = L'\0';
        if (sas_scan_dir(parent, NULL, NULL)) {
            wcscpy(out, parent);
            return 1;
        }
    }

    WCHAR pat[MAX_PATH + 8];
    wsprintfW(pat, L"%s\\*", picked);
    WIN32_FIND_DATAW fd;
    int found = 0, used = 0;
    WCHAR first[MAX_PATH];
    first[0] = L'\0';
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.')
                continue;
            WCHAR child[MAX_PATH + 300];
            wsprintfW(child, L"%s\\%s", picked, fd.cFileName);
            if ((int)wcslen(child) >= MAX_PATH - 8)
                continue;
            if (sas_scan_dir(child, NULL, NULL)) {
                if (found == 0)
                    wcscpy(first, child);
                found++;
                int nl = (int)wcslen(fd.cFileName);
                if (used + nl + 3 < names_cap) {
                    wcscpy(names + used, fd.cFileName);
                    used += nl;
                    names[used++] = L'\n';
                    names[used] = L'\0';
                }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (found == 1) {
        wcscpy(out, first);
        return 1;
    }
    return found > 1 ? -1 : 0;
}

/* Стандартный диалог выбора папки Windows. shell32/ole32 подгружаются на
 * лету (как wininet/gdiplus выше) - ни заголовков, ни ключей линковки не надо.
 * "Новый" вид диалога (с полем для ввода пути) требует CoInitialize; если её
 * не удалось выполнить, используем старый вид - он работает и без неё. */
typedef struct {
    HWND hwndOwner;
    LPVOID pidlRoot;
    LPWSTR pszDisplayName;
    LPCWSTR lpszTitle;
    UINT ulFlags;
    LPVOID lpfn;
    LPARAM lParam;
    int iImage;
} MvBrowseInfo; /* раскладка совпадает с BROWSEINFOW */
typedef LPVOID (WINAPI *PFN_SHBrowseForFolderW)(MvBrowseInfo *);
typedef BOOL (WINAPI *PFN_SHGetPathFromIDListW)(LPVOID, LPWSTR);
typedef LONG (WINAPI *PFN_CoInitialize)(LPVOID);
typedef void (WINAPI *PFN_CoUninitialize)(void);
typedef void (WINAPI *PFN_CoTaskMemFree)(LPVOID);

static int browse_for_folder(HWND hwnd, const WCHAR *title, WCHAR *out) {
    out[0] = L'\0';
    HMODULE sh = LoadLibraryW(L"shell32.dll");
    if (!sh)
        return 0;
    PFN_SHBrowseForFolderW pBrowse =
        (PFN_SHBrowseForFolderW)(void *)GetProcAddress(sh, "SHBrowseForFolderW");
    PFN_SHGetPathFromIDListW pPath =
        (PFN_SHGetPathFromIDListW)(void *)GetProcAddress(sh, "SHGetPathFromIDListW");
    if (!pBrowse || !pPath)
        return 0;

    HMODULE ole = LoadLibraryW(L"ole32.dll");
    PFN_CoInitialize pInit = ole ? (PFN_CoInitialize)(void *)GetProcAddress(ole, "CoInitialize") : NULL;
    PFN_CoUninitialize pUninit =
        ole ? (PFN_CoUninitialize)(void *)GetProcAddress(ole, "CoUninitialize") : NULL;
    PFN_CoTaskMemFree pFree = ole ? (PFN_CoTaskMemFree)(void *)GetProcAddress(ole, "CoTaskMemFree") : NULL;

    int inited = 0;
    if (pInit && pUninit) {
        LONG hr = pInit(NULL);
        inited = (hr == 0 || hr == 1); /* S_OK или S_FALSE - нужен парный CoUninitialize */
    }

    WCHAR display[MAX_PATH];
    display[0] = L'\0';
    MvBrowseInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = hwnd;
    bi.pszDisplayName = display;
    bi.lpszTitle = title;
    bi.ulFlags = 0x0001 /* BIF_RETURNONLYFSDIRS */ | (inited ? (0x0040 | 0x0010) : 0) /* NEWDIALOGSTYLE | EDITBOX */;

    LPVOID pidl = pBrowse(&bi);
    int ok = 0;
    if (pidl) {
        ok = pPath(pidl, out) ? 1 : 0;
        if (pFree)
            pFree(pidl);
    }
    if (inited)
        pUninit();
    return ok && out[0];
}

/* --- Хранение настроек: ключи вида Dir_<id>, Overlay_<id> и StyleId в разделе [Map].
 * Раньше стили хранились по номеру в списке (Style=6, SasDir6=...), а список с тех пор
 * изменился - поэтому читаем и старые ключи, чтобы выбранная папка не потерялась. --- */
static const WCHAR *legacy_style_id(int idx) {
    switch (idx) {
    case 5: return L"sas_webmerc";
    case 6: return L"sas_yandex";
    default: return L"osm";
    }
}

static int legacy_dir_index(const WCHAR *id) {
    if (wcscmp(id, L"sas_webmerc") == 0) return 5;
    if (wcscmp(id, L"sas_yandex") == 0) return 6;
    return -1;
}

static int style_index_from_id(const WCHAR *id) {
    for (int i = 0; i < N_STYLES; i++)
        if (wcscmp(kStyles[i].id, id) == 0)
            return i;
    return -1;
}

static void sas_key(const WCHAR *prefix, int st, WCHAR *out) {
    wsprintfW(out, L"%s_%s", prefix, kStyles[st].id);
}

static void save_sas_dir(int st) {
    WCHAR ini[MAX_PATH + 32], key[64];
    cwd_file(L"acars_settings.ini", ini);
    sas_key(L"Dir", st, key);
    WritePrivateProfileStringW(L"Map", key, g_localdir[st], ini);
    if (kStyles[st].has_overlay) {
        sas_key(L"Overlay", st, key);
        WritePrivateProfileStringW(L"Map", key, g_overlaydir[st], ini);
    }
}

static void load_sas_dir(int st) {
    WCHAR ini[MAX_PATH + 32], key[64];
    cwd_file(L"acars_settings.ini", ini);
    g_localdir[st][0] = L'\0';
    g_overlaydir[st][0] = L'\0';
    sas_key(L"Dir", st, key);
    GetPrivateProfileStringW(L"Map", key, L"", g_localdir[st], MAX_PATH, ini);
    if (!g_localdir[st][0]) {
        int li = legacy_dir_index(kStyles[st].id);
        if (li >= 0) {
            wsprintfW(key, L"SasDir%d", li);
            GetPrivateProfileStringW(L"Map", key, L"", g_localdir[st], MAX_PATH, ini);
        }
    }
    if (kStyles[st].has_overlay) {
        sas_key(L"Overlay", st, key);
        GetPrivateProfileStringW(L"Map", key, L"", g_overlaydir[st], MAX_PATH, ini);
    }
}

/* Пределы масштаба текущего стиля: у обычных - общие, у локального кэша -
 * ровно то, что в нём есть (за пределами были бы одни заглушки). */
static int zoom_lo(void) {
    return (kStyles[g_style].local && g_zmin_s[g_style] >= 0) ? g_zmin_s[g_style] : MAP_MIN_ZOOM;
}
static int zoom_hi(void) {
    return (kStyles[g_style].local && g_zmax_s[g_style] >= 0) ? g_zmax_s[g_style] : MAP_MAX_ZOOM;
}
static void clamp_zoom(void) {
    if (g_zoom < zoom_lo()) g_zoom = zoom_lo();
    if (g_zoom > zoom_hi()) g_zoom = zoom_hi();
}

static void update_style_ui(void) {
    if (g_btnSas)
        ShowWindow(g_btnSas, kStyles[g_style].local ? SW_SHOW : SW_HIDE);
    if (g_btnOvr)
        ShowWindow(g_btnOvr, kStyles[g_style].has_overlay ? SW_SHOW : SW_HIDE);
}

/* Папка прозрачного слоя "Гибрид" (подписи и дороги), который накладывается на основу.
 * 1 - выбор сделан (в том числе "работать без слоя"). */
static int pick_overlay_folder(HWND hwnd, int st) {
    int a = MessageBoxW(
        hwnd,
        L"Теперь укажите папку слоя \"Гибрид\": прозрачные PNG с подписями и дорогами, которые "
        L"накладываются на спутник (в SAS.Planet это слой Google.Гибрид).\n\n"
        L"Да - выбрать папку слоя\n"
        L"Нет - работать без слоя (будет просто спутник)\n"
        L"Отмена - ничего не менять",
        L"Слой подписей", MB_YESNOCANCEL | MB_ICONINFORMATION);
    if (a == IDNO) {
        g_overlaydir[st][0] = L'\0';
        save_sas_dir(st);
        return 1;
    }
    if (a != IDYES)
        return 0;

    WCHAR picked[MAX_PATH], dir[MAX_PATH], names[400];
    if (!browse_for_folder(hwnd, L"Папка слоя \"Гибрид\" (внутри - папки z8, z9, ...)", picked))
        return 0;
    int r = sas_resolve_folder(picked, dir, names, 400);
    if (r < 0) {
        WCHAR m2[700];
        wsprintfW(m2,
                  L"В выбранной папке несколько карт:\n\n%s\n"
                  L"Выберите папку ОДНОГО слоя (внутри неё папки масштабов z8, z9, ...).",
                  names);
        MessageBoxW(hwnd, m2, L"Слой подписей", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    if (r == 0) {
        MessageBoxW(hwnd,
                    L"В выбранной папке (и в её подпапках) не найдено папок масштабов "
                    L"(z8, z9, ...).",
                    L"Слой подписей", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    if (_wcsicmp(dir, g_localdir[st]) == 0) {
        MessageBoxW(hwnd,
                    L"Это та же папка, что и основа (спутник). Нужна папка слоя \"Гибрид\" - "
                    L"отдельная.",
                    L"Слой подписей", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    wcscpy(g_overlaydir[st], dir);
    save_sas_dir(st);
    return 1;
}

/* Спрашивает у пользователя папку карты в кэше SAS.Planet. Основной путь -
 * обычный выбор папки (допускаются типичные промахи, см. sas_resolve_folder);
 * запасной - указать любой файл тайла, папка выводится из его пути. 1 - папка
 * выбрана и в ней есть тайлы. */
static int pick_sas_folder(HWND hwnd, int st) {
    WCHAR msg[1000];
    wsprintfW(msg,
              L"Укажите папку карты в кэше SAS.Planet - ту, внутри которой лежат папки "
              L"масштабов z8, z9, ...\n\n"
              L"Для этого стиля нужны тайлы в проекции:\n%s\n\n"
              L"Проекцию определяет ИСТОЧНИК карты, а не файл. Если выбрать не тот стиль, "
              L"самолёты окажутся смещены по широте примерно на 20 км. Поддерживается "
              L"стандартный файловый кэш SAS.Planet.\n\n"
              L"Да - выбрать папку карты\n"
              L"Нет - вместо этого указать любой файл тайла\n"
              L"Отмена - ничего не менять",
              kStyles[st].proj == GEO_PROJ_ELLIPSOID
                  ? L"эллипсоидальный Меркатор, EPSG:3395 (Яндекс, Космоснимки и др.)"
                  : L"Web Mercator, EPSG:3857 (Google, OpenStreetMap, Bing и др.)");
    int answer = MessageBoxW(hwnd, msg, L"Кэш SAS.Planet", MB_YESNOCANCEL | MB_ICONINFORMATION);
    if (answer != IDYES && answer != IDNO)
        return 0;

    WCHAR dir[MAX_PATH];
    dir[0] = L'\0';
    if (answer == IDYES) {
        WCHAR picked[MAX_PATH];
        if (!browse_for_folder(hwnd, L"Папка карты в кэше SAS.Planet (внутри - папки z8, z9, ...)", picked))
            return 0;
        WCHAR names[400];
        int r = sas_resolve_folder(picked, dir, names, 400);
        if (r < 0) {
            WCHAR m2[700];
            wsprintfW(m2,
                      L"В выбранной папке несколько карт:\n\n%s\n"
                      L"Выберите папку ОДНОЙ из них (внутри неё папки масштабов z8, z9, ...).",
                      names);
            MessageBoxW(hwnd, m2, L"Кэш SAS.Planet", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (r == 0) {
            MessageBoxW(hwnd,
                        L"В выбранной папке (и в её подпапках) не найдено папок масштабов "
                        L"(z8, z9, ...).\n\nВыберите папку именно карты, например "
                        L"...\\cache\\yamapng",
                        L"Кэш SAS.Planet", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
    } else {
        WCHAR file[MAX_PATH];
        file[0] = L'\0';
        OPENFILENAMEW ofn;
        memset(&ofn, 0, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = L"Тайлы (*.jpg;*.png;*.gif)\0*.jpg;*.png;*.gif\0Все файлы (*.*)\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        ofn.lpstrTitle = L"Любой файл тайла из папки карты в кэше SAS.Planet";
        /* диалог меняет рабочую папку процесса - возвращаем (см. open_dialog_keep_cwd в gui_win.c) */
        WCHAR saved_cwd[MAX_PATH];
        saved_cwd[0] = L'\0';
        GetCurrentDirectoryW(MAX_PATH, saved_cwd);
        ofn.Flags |= OFN_NOCHANGEDIR;
        BOOL picked_ok = GetOpenFileNameW(&ofn);
        if (saved_cwd[0])
            SetCurrentDirectoryW(saved_cwd);
        if (!picked_ok)
            return 0;
        if (!sas_map_dir_from_tile(file, dir, MAX_PATH)) {
            MessageBoxW(hwnd,
                        L"Этот файл не похож на тайл из кэша SAS.Planet.\n\n"
                        L"Ожидается путь вида  ...\\<карта>\\z12\\19\\x19141\\9\\y9525.jpg\n"
                        L"Если в SAS.Planet выбран другой тип кэша, скопируйте нужную область в "
                        L"формат \"SAS.Планета\" (Операции с выделенной областью - Копировать).",
                        L"Кэш SAS.Planet", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
    }

    wcscpy(g_localdir[st], dir);
    if (!sas_scan_zooms(st)) {
        g_localdir[st][0] = L'\0';
        MessageBoxW(hwnd, L"В этой папке не найдено ни одной папки масштаба (z1, z2, ...).",
                    L"Кэш SAS.Planet", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    save_sas_dir(st);
    if (kStyles[st].has_overlay && !g_overlaydir[st][0])
        pick_overlay_folder(hwnd, st); /* тут можно отказаться - тогда будет просто основа */
    return 1;
}

static void clear_tile_state(void) {
    EnterCriticalSection(&g_qcs);
    g_qn = 0;
    LeaveCriticalSection(&g_qcs);
    EnterCriticalSection(&g_lqcs);
    g_lqn = 0;
    LeaveCriticalSection(&g_lqcs);
    g_tile_gen++; /* ответы фонового потока на прежние запросы теперь устарели и будут отброшены */
    for (int i = 0; i < TILE_CACHE_N; i++) {
        if (g_tiles[i].used && g_tiles[i].bmp)
            DeleteObject(g_tiles[i].bmp);
        if (g_tiles[i].used && g_tiles[i].fbmp)
            DeleteObject(g_tiles[i].fbmp);
        memset(&g_tiles[i], 0, sizeof(g_tiles[i]));
    }
    g_fb_src_h = NULL;
    InterlockedExchange(&g_net_status, 0);
    g_silver_count = 0;
}

static void save_style(void) {
    WCHAR path[MAX_PATH + 32];
    cwd_file(L"acars_settings.ini", path);
    WritePrivateProfileStringW(L"Map", L"StyleId", kStyles[g_style].id, path);
}

static void switch_style(HWND hwnd, int idx) {
    if (idx < 0 || idx >= N_STYLES || idx == g_style)
        return;
    if (kStyles[idx].local) {
        /* диапазон масштабов пересматриваем каждый раз - в кэш могли дозагрузить новые */
        if (!g_localdir[idx][0] || !sas_scan_zooms(idx)) {
            if (!pick_sas_folder(hwnd, idx)) {
                SendMessageW(g_cmbStyle, CB_SETCURSEL, (WPARAM)g_style, 0);
                return;
            }
        }
    }
    g_style = idx;
    clamp_zoom();
    clear_tile_state(); /* тайлы другого стиля в памяти больше не нужны, очередь - тоже */
    save_style();
    update_style_ui();
    InvalidateRect(hwnd, NULL, FALSE);
}

/* ---------------------------------------------------------------- */
/* Геометрия вида                                                   */
/* ---------------------------------------------------------------- */
static void map_rect(HWND hwnd, RECT *m) {
    GetClientRect(hwnd, m);
    m->top = MAP_TOOLBAR_H;
    m->right -= MAP_PANEL_W;
    if (m->right < m->left + 120)
        m->right = m->left + 120;
    if (m->bottom < m->top + 120)
        m->bottom = m->top + 120;
}

/* Мировые координаты левого верхнего угла карты на экране. */
static void view_origin(const RECT *m, double *ox, double *oy) {
    *ox = MV_WX(g_clon, g_zoom) - (m->right - m->left) / 2.0;
    *oy = MV_WY(g_clat, g_zoom) - (m->bottom - m->top) / 2.0;
}

static void ll_to_screen(const RECT *m, double lat, double lon, int *sx, int *sy) {
    double ox, oy;
    view_origin(m, &ox, &oy);
    *sx = m->left + (int)floor(MV_WX(lon, g_zoom) - ox + 0.5);
    *sy = m->top + (int)floor(MV_WY(lat, g_zoom) - oy + 0.5);
}

/* Меняет масштаб так, чтобы точка (sx, sy) осталась под курсором. */
static void set_zoom_at(const RECT *m, int newz, int sx, int sy) {
    if (newz < zoom_lo()) newz = zoom_lo();
    if (newz > zoom_hi()) newz = zoom_hi();
    if (newz == g_zoom)
        return;
    double ox, oy;
    view_origin(m, &ox, &oy);
    double wx = ox + (sx - m->left), wy = oy + (sy - m->top);
    double lon = MV_LON(wx, g_zoom), lat = MV_LAT(wy, g_zoom);
    g_zoom = newz;
    double nox = MV_WX(lon, g_zoom) - (sx - m->left);
    double noy = MV_WY(lat, g_zoom) - (sy - m->top);
    g_clon = wrap_lon(MV_LON(nox + (m->right - m->left) / 2.0, g_zoom));
    g_clat = clamp_lat_view(MV_LAT(noy + (m->bottom - m->top) / 2.0, g_zoom));
}

static void fit_view(HWND hwnd) {
    RECT m;
    map_rect(hwnd, &m);
    int z;
    geo_fit_view_p(g_ac, g_nac, m.right - m.left, m.bottom - m.top, 70, kStyles[g_style].proj,
                   &g_clat, &g_clon, &z);
    if (z < zoom_lo()) z = zoom_lo();
    if (z > zoom_hi()) z = zoom_hi();
    g_zoom = z;
}

/* --- проигрывание истории: состояние карты на выбранный момент --- */
static MapRangeFn g_range_cb;
static int g_rp_on, g_rp_play, g_rp_drag;
static long g_rp_t, g_rp_min, g_rp_max;
void mapview_set_range_cb(MapRangeFn fn) { g_range_cb = fn; }

static void refresh_data(void) {
    if (!g_provider)
        return;
    g_nac = g_provider(g_ac, MAP_MAX_AIRCRAFT, g_rp_on ? g_rp_t : 0);
    if (g_nac < 0) g_nac = 0;
    if (g_nac > MAP_MAX_AIRCRAFT) g_nac = MAP_MAX_AIRCRAFT;
    for (int i = 0; i < g_nac; i++)
        geo_update_heading(&g_ac[i]);
    if (g_sel_reg[0] && find_sel() < 0)
        g_sel_reg[0] = '\0';
    g_search_cnt = 0;
    for (int i = 0; i < g_nac; i++)
        if (ac_matches_search(&g_ac[i]))
            g_search_cnt++;
}

/* ---------------------------------------------------------------- */
/* Рисование                                                        */
/* ---------------------------------------------------------------- */
/* Контур самолёта (нос вверх, единицы - пиксели при масштабе 1.0). */
static const int PLANE_PTS[][2] = {
    {0, -14}, {2, -10}, {2, -4}, {13, 3}, {13, 6}, {2, 3}, {2, 8}, {5, 11}, {5, 13},
    {0, 12},  {-5, 13}, {-5, 11}, {-2, 8}, {-2, 3}, {-13, 6}, {-13, 3}, {-2, -4}, {-2, -10}};
#define PLANE_N ((int)(sizeof(PLANE_PTS) / sizeof(PLANE_PTS[0])))

/* У каждого борта свой цвет - значок, след и подпись на карте и квадратик в списке справа одного цвета, поэтому
 * видно, какой след чей. Цвет определяется номером борта в списке (список растёт по порядку появления бортов,
 * так что цвет не прыгает). Оттенки подобраны так, чтобы различаться и на светлой, и на тёмной подложке. */
static COLORREF plane_color(int idx) {
    static const COLORREF pal[] = {
        RGB(255, 70, 70),   RGB(0, 205, 255),  RGB(120, 255, 60),  RGB(255, 225, 0),
        RGB(255, 80, 200),  RGB(170, 110, 255), RGB(255, 140, 20), RGB(40, 240, 170),
        RGB(255, 255, 255), RGB(80, 140, 255),  RGB(255, 170, 140), RGB(190, 255, 0)};
    return pal[(idx < 0 ? 0 : idx) % (int)(sizeof(pal) / sizeof(pal[0]))];
}

static void draw_plane(HDC dc, int cx, int cy, double heading_deg, double scale, COLORREF fill, COLORREF edge) {
    POINT pts[PLANE_N];
    double th = heading_deg * 3.14159265358979323846 / 180.0;
    double c = cos(th), s = sin(th);
    for (int i = 0; i < PLANE_N; i++) {
        double px = PLANE_PTS[i][0] * scale, py = PLANE_PTS[i][1] * scale;
        pts[i].x = cx + (LONG)floor(px * c - py * s + 0.5);
        pts[i].y = cy + (LONG)floor(px * s + py * c + 0.5);
    }
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pen);
    Polygon(dc, pts, PLANE_N);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

static void draw_graticule(HDC dc, const RECT *m, double ox, double oy) {
    static const double steps[] = {0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1,  2,
                                   5,     10,   15,   30,   45};
    double ppd = geo_world_size(g_zoom) / 360.0;
    double step = 45.0;
    for (int i = 0; i < (int)(sizeof(steps) / sizeof(steps[0])); i++) {
        if (steps[i] * ppd >= 90.0) {
            step = steps[i];
            break;
        }
    }
    int w = m->right - m->left, h = m->bottom - m->top;
    double lonL = MV_LON(ox, g_zoom), lonR = MV_LON(ox + w, g_zoom);
    double latT = MV_LAT(oy, g_zoom), latB = MV_LAT(oy + h, g_zoom);

    HPEN pen = CreatePen(PS_SOLID, 1, CLR_TILE_LINE);
    HGDIOBJ op = SelectObject(dc, pen);
    SetTextColor(dc, CLR_DIM);
    SelectObject(dc, g_fontSmall);
    char lab[32];
    int guard = 0;
    for (double lon = floor(lonL / step) * step; lon <= lonR && guard < 200; lon += step, guard++) {
        int sx = m->left + (int)(MV_WX(lon, g_zoom) - ox);
        MoveToEx(dc, sx, m->top, NULL);
        LineTo(dc, sx, m->bottom);
        snprintf(lab, sizeof(lab), "%.4g E", lon);
        put_text(dc, sx + 3, m->top + 3, lab);
    }
    guard = 0;
    for (double lat = floor(latB / step) * step; lat <= latT && guard < 200; lat += step, guard++) {
        int sy = m->top + (int)(MV_WY(lat, g_zoom) - oy);
        MoveToEx(dc, m->left, sy, NULL);
        LineTo(dc, m->right, sy);
        snprintf(lab, sizeof(lab), "%.4g N", lat);
        put_text(dc, m->left + 3, sy - 14, lab);
    }
    SelectObject(dc, op);
    DeleteObject(pen);
}

/* Растягивает участок size x size с левым верхним углом (sx, sy) тайла src (256x256, BGRA) до целого
 * тайла 256x256 с БИЛИНЕЙНОЙ интерполяцией: каждый пиксель результата - взвешенное среднее четырёх ближайших
 * пикселей исходника. Обычное растяжение (StretchBlt) просто повторяет пиксели блоками - при увеличении в 4-16 раз
 * это "кубики", особенно заметные на подписях. Пиксели за краем участка берутся из соседних пикселей самого
 * тайла-предка (а не повторяются), поэтому швов между соседними тайлами нет; зажимается только край предка. */
static void bilinear_crop_scale(const BYTE *src, int sx, int sy, int size, BYTE *dst) {
    int x0[TILE_SIZE], x1[TILE_SIZE], wx[TILE_SIZE], y0[TILE_SIZE], y1[TILE_SIZE], wy[TILE_SIZE];
    for (int i = 0; i < TILE_SIZE; i++) {
        double u = sx + ((i + 0.5) * size / TILE_SIZE - 0.5);
        double v = sy + ((i + 0.5) * size / TILE_SIZE - 0.5);
        if (u < 0) u = 0;
        if (u > TILE_SIZE - 1) u = TILE_SIZE - 1;
        if (v < 0) v = 0;
        if (v > TILE_SIZE - 1) v = TILE_SIZE - 1;
        int fu = (int)u, fv = (int)v;
        x0[i] = fu;
        x1[i] = fu + 1 < TILE_SIZE ? fu + 1 : fu;
        wx[i] = (int)((u - fu) * 256.0 + 0.5);
        y0[i] = fv;
        y1[i] = fv + 1 < TILE_SIZE ? fv + 1 : fv;
        wy[i] = (int)((v - fv) * 256.0 + 0.5);
    }
    for (int y = 0; y < TILE_SIZE; y++) {
        const BYTE *r0 = src + (size_t)y0[y] * TILE_SIZE * 4;
        const BYTE *r1 = src + (size_t)y1[y] * TILE_SIZE * 4;
        int wy1 = wy[y], wy0 = 256 - wy1;
        BYTE *d = dst + (size_t)y * TILE_SIZE * 4;
        for (int x = 0; x < TILE_SIZE; x++, d += 4) {
            int wx1 = wx[x], wx0 = 256 - wx1;
            const BYTE *p00 = r0 + x0[x] * 4, *p10 = r0 + x1[x] * 4;
            const BYTE *p01 = r1 + x0[x] * 4, *p11 = r1 + x1[x] * 4;
            for (int c = 0; c < 3; c++)
                d[c] = (BYTE)((p00[c] * wx0 * wy0 + p10[c] * wx1 * wy0 + p01[c] * wx0 * wy1 + p11[c] * wx1 * wy1 + 32768) >> 16);
            d[3] = 255;
        }
    }
}

/* Что рисовать на месте тайла, которого в кэше нет совсем (область скачана на мелком масштабе, на крупном - нет):
 *   0 - ничего, просто фон (честно: деталей нет);
 *   1 - растянутый тайл мелкого масштаба, но ПРИГЛУШЁННЫЙ (смешан с фоном) - заплатка не бросается в глаза;
 *   2 - растянутый тайл как есть.
 * Подписи в таких тайлах нарисованы в самой картинке (спутник с подписями), поэтому растягиваются вместе с ней
 * и выглядят в 2-4 раза крупнее настоящих - режим 1 делает их менее заметными, режим 0 убирает совсем.
 * Переключается клавишей M, запоминается в acars_settings.ini ([Map] FillMode). */
static int g_fill_mode = 2;
static const char *const FILL_NAMES[] = {"без картинки", "приглушённо", "как есть"};

/* Смешивает картинку BGRA с цветом bg: pct процентов цвета фона (0 - без изменений, 100 - сплошной фон). */
static void dim_toward_bg(BYTE *px, int npix, COLORREF bg, int pct) {
    int br = GetRValue(bg), bgc = GetGValue(bg), bb = GetBValue(bg);
    for (int i = 0; i < npix; i++, px += 4) {
        px[0] = (BYTE)((px[0] * (100 - pct) + bb * pct + 50) / 100);
        px[1] = (BYTE)((px[1] * (100 - pct) + bgc * pct + 50) / 100);
        px[2] = (BYTE)((px[2] * (100 - pct) + br * pct + 50) / 100);
    }
}

#define FB_BUDGET 6
static int g_fb_budget; /* сколько сглаженных подстановок ещё можно построить в этом кадре (каждая ~1 мс) */
static int g_fb_more;   /* 1 - бюджета не хватило, нужен ещё один кадр */
static BYTE g_fb_pix[TILE_SIZE * TILE_SIZE * 4];

/* Читает пиксели готового тайла в g_fb_pix (32 бита, строки сверху вниз). Соседние тайлы чаще всего
 * строятся из одного и того же предка, поэтому прочитанное запоминается. */
static int read_tile_pixels(HBITMAP hb) {
    if (hb == g_fb_src_h)
        return 1;
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = TILE_SIZE;
    bmi.bmiHeader.biHeight = -TILE_SIZE;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(NULL);
    if (!dc)
        return 0;
    int lines = GetDIBits(dc, hb, 0, TILE_SIZE, g_fb_pix, &bmi, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);
    if (lines != TILE_SIZE) {
        g_fb_src_h = NULL;
        return 0;
    }
    g_fb_src_h = hb;
    return 1;
}

/* Сглаженная подстановка для тайла, которого в кэше нет совсем (долго живущая "дыра"): строится один раз
 * из ближайшего готового тайла другого масштаба и хранится в ячейке. NULL - строить не из чего или в этом
 * кадре кончился бюджет (тогда на кадр ставится быстрая подстановка, а следующий кадр достроит). */
static HBITMAP tile_smooth_bmp(int z, int x, int y) {
    TileSlot *s = tile_find(g_style, z, x, y);
    if (!s)
        return NULL;
    for (int k = 1; k <= 4 && z - k >= 0; k++) {
        int ax, ay, sx, sy, size;
        ancestor_src(x, y, k, &ax, &ay, &sx, &sy, &size);
        HBITMAP src = tile_peek_bmp(z - k, ax, ay);
        if (!src)
            continue;
        if (s->fbmp && s->fb_k == k) /* уже построена из этого же предка */
            return s->fbmp;
        if (g_fb_budget <= 0) {
            g_fb_more = 1;
            return NULL;
        }
        if (!read_tile_pixels(src))
            return NULL;
        BITMAPINFO bmi;
        memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = TILE_SIZE;
        bmi.bmiHeader.biHeight = -TILE_SIZE;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void *bits = NULL;
        HBITMAP hb = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!hb || !bits) {
            if (hb)
                DeleteObject(hb);
            return NULL;
        }
        bilinear_crop_scale(g_fb_pix, sx, sy, size, (BYTE *)bits);
        if (g_fill_mode == 1)
            dim_toward_bg((BYTE *)bits, TILE_SIZE * TILE_SIZE, CLR_MAPBG, 40);
        g_fb_budget--;
        if (s->fbmp)
            DeleteObject(s->fbmp); /* раньше была построена из более далёкого предка - теперь есть ближе */
        s->fbmp = hb;
        s->fb_k = k;
        return hb;
    }
    return NULL;
}

/* Градусы -> запись в формате SAS.Planet: N45°34'48.00" (градусы, минуты, секунды). Минуты и секунды здесь
 * ВСЕГДА меньше 60: считаем в сотых долях секунды целым числом, так что округление само переносит в старший
 * разряд (44,99999999 -> 45°0'0.00", а не 44°60'0.00"). Именно такая ошибка ("44°96'") не принималась
 * диалогом "Выделение по координатам". pos/neg - буквы для положительного/отрицательного значения. */
static void format_dms(double deg, char pos, char neg, char *out, size_t cap) {
    char letter = deg < 0 ? neg : pos;
    double a = fabs(deg);
    long long tot = (long long)floor(a * 360000.0 + 0.5); /* сотые доли секунды */
    long long d = tot / 360000, rem = tot % 360000, m = rem / 6000, s100 = rem % 6000;
    snprintf(out, cap, "%c%lld\302\260%lld'%lld.%02lld\"", letter, d, m, s100 / 100, s100 % 100);
}

/* Углы области для SAS.Planet в том виде, как их вводить в диалог "Выделение по координатам": верхний левый -
 * северо-западный угол (северная широта, западная долгота), нижний правый - юго-восточный. */
static void format_missing_dms(char *out, size_t cap, int z, int x0, int x1, int y0, int y1, int proj) {
    char a[40], b[40], c[40], d[40];
    format_dms(geo_lat_from_yp((double)y0 * TILE_SIZE, z, proj), 'N', 'S', a, sizeof(a));
    format_dms(geo_lon_from_xp((double)x0 * TILE_SIZE, z, proj), 'E', 'W', b, sizeof(b));
    format_dms(geo_lat_from_yp((double)(y1 + 1) * TILE_SIZE, z, proj), 'N', 'S', c, sizeof(c));
    format_dms(geo_lon_from_xp((double)(x1 + 1) * TILE_SIZE, z, proj), 'E', 'W', d, sizeof(d));
    snprintf(out, cap, "Для SAS.Planet (Выделение по координатам): верхний левый угол  %s  %s    нижний правый угол  %s  %s", a, b, c, d);
}

/* Забыть все построенные подстановки (после смены режима их надо построить заново). */
static void drop_all_fallbacks(void) {
    for (int i = 0; i < TILE_CACHE_N; i++) {
        if (g_tiles[i].used && g_tiles[i].fbmp) {
            DeleteObject(g_tiles[i].fbmp);
            g_tiles[i].fbmp = NULL;
        }
    }
}

/* Подсказка про недостающую область: сколько тайлов не хватает и какие координаты ввести в SAS.Planet, чтобы
 * докачать ровно её. z - масштаб программы (в SAS.Planet он на единицу больше). Углы области переводятся в
 * широту/долготу той же проекцией, что и карта, поэтому подходят для любого стиля. */
static void format_missing_area(char *out, size_t cap, int n_miss, int n_total, int z, int x0, int x1, int y0, int y1,
                                int proj, int mode) {
    double lat_n = geo_lat_from_yp((double)y0 * TILE_SIZE, z, proj);
    double lat_s = geo_lat_from_yp((double)(y1 + 1) * TILE_SIZE, z, proj);
    double lon_w = geo_lon_from_xp((double)x0 * TILE_SIZE, z, proj);
    double lon_e = geo_lon_from_xp((double)(x1 + 1) * TILE_SIZE, z, proj);
    snprintf(out, cap,
             "Не хватает %d из %d тайлов, масштаб SAS.Planet %d. Область для докачки: широта %.2f ... %.2f, "
             "долгота %.2f ... %.2f.  Пустые места: %s (клавиша M)",
             n_miss, n_total, z + 1, lat_s, lat_n, lon_w, lon_e, FILL_NAMES[mode < 0 || mode > 2 ? 2 : mode]);
}

/* Тайлы запаса: рамка шириной margin вокруг видимого прямоугольника [tx0..tx1] x [ty0..ty1] масштаба с
 * n тайлами по стороне. x заворачивается по кругу (по долготе карта замкнута), y обрезается краями мира.
 * Если кадр вместе с запасом шире мира, запас не нужен (завернулся бы на видимые тайлы). Возвращает число. */
static int ring_tiles(int tx0, int tx1, int ty0, int ty1, int n, int margin, int out[][2], int cap) {
    int count = 0;
    if ((tx1 - tx0 + 1) + 2 * margin >= n)
        return 0;
    for (int ty = ty0 - margin; ty <= ty1 + margin; ty++) {
        if (ty < 0 || ty >= n)
            continue;
        for (int tx = tx0 - margin; tx <= tx1 + margin; tx++) {
            if (tx >= tx0 && tx <= tx1 && ty >= ty0 && ty <= ty1)
                continue;
            if (count >= cap)
                return count;
            out[count][0] = ((tx % n) + n) % n;
            out[count][1] = ty;
            count++;
        }
    }
    return count;
}

/* ---- подписи точек следа ---- */
static int rects_hit(const RECT *a, const RECT *b) {
    return a->left < b->right && b->left < a->right && a->top < b->bottom && b->top < a->bottom;
}
static int label_free(const RECT *r, const RECT *list, int n) {
    for (int i = 0; i < n; i++)
        if (rects_hit(r, &list[i])) return 0;
    return 1;
}
static void time_short(const char *t, char *out, size_t cap) { /* "2026-10-06 12:42:52" -> "12:42:52" */
    const char *sp = strrchr(t, ' ');
    snprintf(out, cap, "%.20s", sp ? sp + 1 : t);
}
static int cur_point(const MapAircraft *a) {
    if (a->n_trail <= 0) return -1;
    return (g_sel_pt >= 0 && g_sel_pt < a->n_trail) ? g_sel_pt : a->n_trail - 1;
}

/* Точки следа выбранного борта: пронумерованные кружки (1 - самая старая позиция) и рядом подпись с координатами
 * и временем - так маршрут читается прямо на карте. Подписи, которые наезжают друг на друга, пропускаются
 * (кроме выбранной точки) - полный список всегда есть в панели справа. */
static void draw_track_points(HDC dc, const RECT *m, int i, COLORREF col, RECT *lbl, int *nlbl, int lbl_max) {
    const MapAircraft *a = &g_ac[i];
    int cur = cur_point(a);
    for (int k = 0; k < a->n_trail; k++) {
        int sx, sy;
        ll_to_screen(m, a->trail_lat[k], a->trail_lon[k], &sx, &sy);
        if (sx < m->left - 40 || sx > m->right + 40 || sy < m->top - 40 || sy > m->bottom + 40)
            continue;
        int last = (k == a->n_trail - 1);
        if (!last) {
            HBRUSH br = CreateSolidBrush(col);
            HPEN pen = CreatePen(PS_SOLID, k == cur ? 3 : 1, k == cur ? RGB(255, 255, 255) : RGB(0, 0, 0));
            HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
            Ellipse(dc, sx - 9, sy - 9, sx + 10, sy + 10);
            SelectObject(dc, ob); SelectObject(dc, op);
            DeleteObject(br); DeleteObject(pen);
            WCHAR num[8];
            wsprintfW(num, L"%d", k + 1);
            RECT nr = {sx - 9, sy - 9, sx + 10, sy + 10};
            SetTextColor(dc, RGB(0, 0, 0));
            DrawTextW(dc, num, -1, &nr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
        } else if (k == cur) {
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Ellipse(dc, sx - 14, sy - 14, sx + 15, sy + 15);
            SelectObject(dc, ob); SelectObject(dc, op);
            DeleteObject(pen);
        }

        char tm[24], lab[120];
        time_short(a->trail_time[k], tm, sizeof tm);
        snprintf(lab, sizeof lab, "%d   %.3f%c  %.3f%c   %.20s", k + 1, fabs(a->trail_lat[k]),
                 a->trail_lat[k] >= 0 ? 'N' : 'S', fabs(a->trail_lon[k]), a->trail_lon[k] >= 0 ? 'E' : 'W', tm);
        WCHAR wl[120];
        u8_to_w(lab, wl, 120);
        SIZE sz;
        GetTextExtentPoint32W(dc, wl, (int)wcslen(wl), &sz);
        int w = sz.cx + 8, h = sz.cy + 2;
        int dx = last ? 18 : 13;
        int cand[4][2] = {{sx + dx, sy + (last ? 6 : -h - 2)}, {sx + dx, sy + (last ? 24 : 4)},
                          {sx - dx - w, sy + (last ? 6 : -h - 2)}, {sx - dx - w, sy + (last ? 24 : 4)}};
        RECT lr = {0, 0, 0, 0};
        int ok = 0;
        for (int c = 0; c < 4 && !ok; c++) {
            RECT t = {cand[c][0], cand[c][1], cand[c][0] + w, cand[c][1] + h};
            if (label_free(&t, lbl, *nlbl)) { lr = t; ok = 1; }
        }
        if (!ok && k == cur) {
            RECT t = {cand[0][0], cand[0][1], cand[0][0] + w, cand[0][1] + h};
            lr = t; ok = 1;
        }
        if (!ok) continue;
        HBRUSH lb = CreateSolidBrush(CLR_PANEL);
        FillRect(dc, &lr, lb);
        DeleteObject(lb);
        SetTextColor(dc, k == cur ? CLR_AMBER_SEL : col);
        TextOutW(dc, lr.left + 4, lr.top + 1, wl, (int)wcslen(wl));
        if (*nlbl < lbl_max) lbl[(*nlbl)++] = lr;
    }
}

/* Карточка выбранной точки следа: всё, что передавалось в сообщении с этой позицией. Рисуется в левом
 * нижнем углу карты. */
static void draw_point_card(HDC dc, const RECT *m, const MapAircraft *a, int k, COLORREF col) {
    char body[2048];
    int n = acars_report_point(a->trail_label[k], a->trail_text[k], body, sizeof(body));
    char lines[24][200];
    int nl = 0;
    snprintf(lines[nl++], 200, "%.23s  -  точка %d из %d", a->reg, k + 1, a->n_trail);
    snprintf(lines[nl++], 200, "Принято: %.40s", a->trail_time[k]);
    snprintf(lines[nl++], 200, "Позиция: %.4f %c, %.4f %c", fabs(a->trail_lat[k]), a->trail_lat[k] >= 0 ? 'N' : 'S',
             fabs(a->trail_lon[k]), a->trail_lon[k] >= 0 ? 'E' : 'W');
    {
        char lab[8];
        snprintf(lab, sizeof lab, "%.3s", a->trail_label[k]);
        snprintf(lines[nl++], 200, "Метка %s: %.150s", lab, acars_label_lookup(a->trail_label[k]));
    }
    if (n > 0) {
        const char *p = body;
        while (*p && nl < 24) {
            size_t l = strcspn(p, "\n");
            snprintf(lines[nl++], 200, "%.*s", (int)(l > 190 ? 190 : l), p);
            p += l;
            if (*p == '\n') p++;
        }
    } else {
        snprintf(lines[nl++], 200, "Формат сообщения не расшифрован. Известно только то, что вынесено выше");
        if (a->trail_alt[k][0] && nl < 24) { char at[48]; alt_ft_text(a->trail_alt[k], at, sizeof at, NULL); snprintf(lines[nl++], 200, "Высота: %s", at); }
        if (a->trail_fuel[k][0] && nl < 24) snprintf(lines[nl++], 200, "Топливо: %.15s", a->trail_fuel[k]);
        if (a->trail_eta[k][0] && nl < 24) snprintf(lines[nl++], 200, "ETA: %.15s", a->trail_eta[k]);
    }
    const int LH = 17, W = 400;
    int H = nl * LH + 14;
    int avail_w = (m->right - m->left) - 20;
    int w = W < avail_w ? W : avail_w;
    RECT card = {m->left + 10, m->bottom - H - 10, m->left + 10 + w, m->bottom - 10};
    if (card.top < m->top + 4) card.top = m->top + 4;
    HBRUSH bb = CreateSolidBrush(CLR_PANEL);
    FillRect(dc, &card, bb);
    DeleteObject(bb);
    HPEN bp = CreatePen(PS_SOLID, 2, col);
    HGDIOBJ op = SelectObject(dc, bp), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, card.left, card.top, card.right, card.bottom);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(bp);
    SelectObject(dc, g_fontSmall);
    for (int i = 0; i < nl; i++) {
        RECT lr = {card.left + 8, card.top + 7 + i * LH, card.right - 6, card.top + 7 + (i + 1) * LH};
        if (lr.bottom > card.bottom) break;
        SetTextColor(dc, i == 0 ? col : (i <= 3 ? CLR_DIM : CLR_TEXT));
        WCHAR wl[200];
        u8_to_w(lines[i], wl, 200);
        DrawTextW(dc, wl, -1, &lr, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}


/* ---- маршрут выбранного борта: аэропорты вылета/назначения ---- */
static MapReportFn g_report_cb;
static RECT g_rep_rc;
static int g_rep_click;

void mapview_set_report_cb(MapReportFn fn) { g_report_cb = fn; }

static int route_airport(const char *icao, AirportInfo *ap) {
    memset(ap, 0, sizeof *ap);
    if (!icao || !icao[0])
        return 0;
    return airport_db_lookup(icao, ap) && ap->has_coord;
}

/* "UIII, Иркутск (Irkutsk International Airport)" - сколько известно */
static void airport_line(const char *icao, char *out, size_t cap) {
    AirportInfo ap;
    if (airport_db_lookup(icao, &ap)) {
        if (ap.municipality[0])
            snprintf(out, cap, "%s, %.40s (%.60s)", icao, ap.municipality, ap.name);
        else
            snprintf(out, cap, "%s (%.60s)", icao, ap.name);
    } else
        snprintf(out, cap, "%s", icao);
}

static void draw_airport_marker(HDC dc, int sx, int sy, const char *icao, const char *extra, COLORREF col, int big,
                                RECT *lbl, int *nlbl) {
    int r = big ? 9 : 5;
    HBRUSH br = CreateSolidBrush(big ? RGB(20, 24, 30) : RGB(60, 66, 76));
    HPEN pen = CreatePen(PS_SOLID, big ? 2 : 1, col);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    POINT d[4] = {{sx, sy - r}, {sx + r, sy}, {sx, sy + r}, {sx - r, sy}};
    Polygon(dc, d, 4);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
    char txt[96];
    if (extra && extra[0])
        snprintf(txt, sizeof txt, "%.4s  %.80s", icao, extra);
    else
        snprintf(txt, sizeof txt, "%.4s", icao);
    WCHAR w[96];
    u8_to_w(txt, w, 96);
    SIZE sz;
    GetTextExtentPoint32W(dc, w, (int)wcslen(w), &sz);
    RECT lr = {sx + r + 3, sy - sz.cy / 2 - 1, sx + r + 3 + sz.cx + 8, sy + sz.cy / 2 + 2};
    if (!big && !label_free(&lr, lbl, *nlbl))
        return; /* лишние подписи не нагромождаем */
    if (*nlbl < 400)
        lbl[(*nlbl)++] = lr;
    HBRUSH lb = CreateSolidBrush(CLR_PANEL);
    FillRect(dc, &lr, lb);
    DeleteObject(lb);
    SetTextColor(dc, col);
    TextOutW(dc, lr.left + 4, lr.top + 1, w, (int)wcslen(w));
}

/* Все известные аэропорты (встроенная таблица/airport_db.csv) и аэропорты вылета/назначения бортов - серые ромбы.
 * Включается клавишей A. Подписи кодов - только если хватает масштаба и места. */
static int g_show_airports = 1;

static void draw_all_airports(HDC dc, const RECT *m, RECT *lbl, int *nlbl) {
    static AirportInfo list[64];
    static int nlist = -1;
    if (nlist < 0)
        nlist = airport_db_map_list(list, 64);
    SelectObject(dc, g_fontSmall);
    char seen[128][5];
    int nseen = 0;
    for (int i = 0; i < nlist + 2 * g_nac; i++) {
        AirportInfo ap;
        if (i < nlist)
            ap = list[i];
        else {
            const MapAircraft *a = &g_ac[(i - nlist) / 2];
            const char *code = ((i - nlist) % 2) ? a->dest : a->dep;
            if (!code[0] || !route_airport(code, &ap))
                continue;
        }
        int dup = 0;
        for (int k = 0; k < nseen; k++)
            if (!strcmp(seen[k], ap.icao)) { dup = 1; break; }
        if (dup || nseen >= 128)
            continue;
        {
            int si = find_sel();
            if (si >= 0 && (!strcmp(g_ac[si].dep, ap.icao) || !strcmp(g_ac[si].dest, ap.icao)))
                continue; /* аэропорты маршрута выбранного борта рисуются крупно отдельно */
        }
        snprintf(seen[nseen++], 5, "%.4s", ap.icao);
        int sx, sy;
        ll_to_screen(m, ap.lat, ap.lon, &sx, &sy);
        if (sx < m->left - 20 || sx > m->right + 20 || sy < m->top - 20 || sy > m->bottom + 20)
            continue;
        draw_airport_marker(dc, sx, sy, ap.icao, NULL, RGB(150, 160, 175), 0, lbl, nlbl);
    }
}

/* Маршрут выбранного борта на карте: крупные ромбы аэропортов вылета и назначения. Линий нет: прямая от самолёта до
 * аэропорта назначения вводила в заблуждение (рейсы идут по трассам, с обходами), пройденный путь показывает след.
 * У аэропорта назначения подписано расстояние по прямой. */
static void draw_route_overlay(HDC dc, const RECT *m, int i, RECT *lbl, int *nlbl) {
    const MapAircraft *a = &g_ac[i];
    AirportInfo ad, aa;
    int hd = route_airport(a->dep, &ad), ha = route_airport(a->dest, &aa);
    if (!hd && !ha)
        return;
    COLORREF col = plane_color(i);
    SelectObject(dc, g_fontSmall);
    if (hd) {
        int x, y;
        ll_to_screen(m, ad.lat, ad.lon, &x, &y);
        draw_airport_marker(dc, x, y, a->dep, "вылет", col, 1, lbl, nlbl);
    }
    if (ha) {
        int x, y;
        char ex[64];
        snprintf(ex, sizeof ex, "назначение, %.0f км по прямой", geo_distance_km(a->lat, a->lon, aa.lat, aa.lon));
        ll_to_screen(m, aa.lat, aa.lon, &x, &y);
        draw_airport_marker(dc, x, y, a->dest, ex, col, 1, lbl, nlbl);
    }
}

static void draw_map(HDC dc, const RECT *m) {
    int w = m->right - m->left, h = m->bottom - m->top;
    SaveDC(dc);
    IntersectClipRect(dc, m->left, m->top, m->right, m->bottom);

    HBRUSH bg = CreateSolidBrush(CLR_MAPBG);
    FillRect(dc, m, bg);
    DeleteObject(bg);

    double ox, oy;
    view_origin(m, &ox, &oy);
    int n = 1 << g_zoom;
    int tx0 = (int)floor(ox / TILE_SIZE), tx1 = (int)floor((ox + w) / TILE_SIZE);
    int ty0 = (int)floor(oy / TILE_SIZE), ty1 = (int)floor((oy + h) / TILE_SIZE);

    HDC tdc = CreateCompatibleDC(dc);
    int drawn = 0;
    g_view_total = g_view_have = g_view_pending = g_view_miss_have = 0;
    g_view_miss_n = 0;
    g_fb_budget = FB_BUDGET;
    g_fb_more = 0;
    HBRUSH ph = CreateSolidBrush(CLR_TILE_PH);  /* тайла в кэше нет совсем */
    HBRUSH bgb = CreateSolidBrush(CLR_MAPBG);   /* тайл ещё грузится - сливается с фоном, не бросается в глаза */
    for (int ty = ty0; ty <= ty1; ty++) {
        if (ty < 0 || ty >= n)
            continue;
        for (int tx = tx0; tx <= tx1; tx++) {
            int txw = ((tx % n) + n) % n;
            int dx = m->left + (int)floor(tx * (double)TILE_SIZE - ox + 0.5);
            int dy = m->top + (int)floor(ty * (double)TILE_SIZE - oy + 0.5);
            int st = TS_EMPTY;
            HBITMAP hb = tile_get(g_zoom, txw, ty, 0, &st);
            g_view_total++;
            if (hb) {
                HGDIOBJ old = SelectObject(tdc, hb);
                BitBlt(dc, dx, dy, TILE_SIZE, TILE_SIZE, tdc, 0, 0, SRCCOPY);
                SelectObject(tdc, old);
                drawn++;
                g_view_have++;
            } else if (st == TS_FAILED) { /* такого тайла в кэше нет */
                if (!g_view_miss_have) { /* запоминаем первый ненайденный тайл кадра */
                    g_view_miss_have = 1;
                    g_view_miss_z = g_zoom;
                    g_view_miss_x = txw;
                    g_view_miss_y = ty;
                    g_view_miss_x0 = g_view_miss_x1 = txw;
                    g_view_miss_y0 = g_view_miss_y1 = ty;
                } else { /* и габариты всех недостающих */
                    if (txw < g_view_miss_x0) g_view_miss_x0 = txw;
                    if (txw > g_view_miss_x1) g_view_miss_x1 = txw;
                    if (ty < g_view_miss_y0) g_view_miss_y0 = ty;
                    if (ty > g_view_miss_y1) g_view_miss_y1 = ty;
                }
                g_view_miss_n++;
                HBITMAP fb = g_fill_mode == 0 ? NULL : tile_smooth_bmp(g_zoom, txw, ty);
                if (g_fill_mode == 0) {
                    RECT hole = {dx, dy, dx + TILE_SIZE, dy + TILE_SIZE};
                    FillRect(dc, &hole, ph);
                } else if (fb) { /* долгая "дыра" в кэше - показываем плавно растянутый тайл другого масштаба */
                    HGDIOBJ old = SelectObject(tdc, fb);
                    BitBlt(dc, dx, dy, TILE_SIZE, TILE_SIZE, tdc, 0, 0, SRCCOPY);
                    SelectObject(tdc, old);
                } else {
                    draw_tile_fallback(dc, tdc, g_zoom, txw, ty, dx, dy, ph);
                }
            } else { /* грузится: пока показываем растянутый тайл другого масштаба, если он есть */
                g_view_pending++;
                draw_tile_fallback(dc, tdc, g_zoom, txw, ty, dx, dy, bgb);
            }
        }
    }
    if (kStyles[g_style].local) { /* запас по краям - заранее, чтобы при сдвиге новые полосы уже были готовы */
        int ring[256][2];
        int nr = ring_tiles(tx0, tx1, ty0, ty1, n, 1, ring, 256);
        for (int i = 0; i < nr; i++)
            tile_prefetch(g_zoom, ring[i][0], ring[i][1]);
    }
    DeleteObject(ph);
    DeleteObject(bgb);
    DeleteDC(tdc);
    if (g_fb_more && g_hwndMap) { /* не все плавные подстановки успели построиться - доделаем в следующем кадре */
        RECT again = *m;
        InvalidateRect(g_hwndMap, &again, FALSE);
    }

    SetBkMode(dc, TRANSPARENT);
    if (!drawn && g_view_pending == 0)
        draw_graticule(dc, m, ox, oy);

    /* следы: линия и точки цветом борта */
    if (g_show_trails) {
        int sel_t = find_sel();
        for (int i = 0; i < g_nac; i++) {
            if (g_ac[i].n_trail < 1)
                continue;
            COLORREF col = plane_color(i);
            POINT pts[MAP_MAX_TRAIL];
            int np = 0;
            for (int k = 0; k < g_ac[i].n_trail && np < MAP_MAX_TRAIL; k++) {
                int sx, sy;
                ll_to_screen(m, g_ac[i].trail_lat[k], g_ac[i].trail_lon[k], &sx, &sy);
                pts[np].x = sx;
                pts[np].y = sy;
                np++;
            }
            HPEN tp = CreatePen(PS_SOLID, i == sel_t ? 3 : 2, col);
            HGDIOBJ op = SelectObject(dc, tp);
            if (np >= 2)
                Polyline(dc, pts, np);
            SelectObject(dc, op);
            DeleteObject(tp);
            HBRUSH db = CreateSolidBrush(col);
            HPEN bp = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
            HGDIOBJ ob = SelectObject(dc, db);
            op = SelectObject(dc, bp);
            for (int k = 0; k < np - 1; k++)
                Ellipse(dc, pts[k].x - 3, pts[k].y - 3, pts[k].x + 4, pts[k].y + 4);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(db);
            DeleteObject(bp);
        }
    }

    /* самолёты и подписи */
    SelectObject(dc, g_fontSmall);
    int sel = find_sel();
    RECT lbl[400];
    int nlbl = 0;
    if (g_show_airports)
        draw_all_airports(dc, m, lbl, &nlbl);
    if (sel >= 0)
        draw_route_overlay(dc, m, sel, lbl, &nlbl); /* аэропорты маршрута - под самолётами */
    for (int pass = 0; pass < 2; pass++) { /* выбранный рисуем последним - поверх остальных */
        if (pass == 1)
            nlbl = 0; /* подписи выбранного борта и его следа не должны уступать чужим */
        for (int i = 0; i < g_nac; i++) {
            int is_sel = (i == sel);
            if ((pass == 0) == is_sel)
                continue;
            int sx, sy;
            ll_to_screen(m, g_ac[i].lat, g_ac[i].lon, &sx, &sy);
            if (sx < m->left - 60 || sx > m->right + 60 || sy < m->top - 60 || sy > m->bottom + 60)
                continue;

            COLORREF col = plane_color(i);
            int is_hit = !is_sel && ac_matches_search(&g_ac[i]);
            if (is_hit) { /* найденный по запросу борт: жёлтое кольцо */
                HPEN rp = CreatePen(PS_SOLID, 3, RGB(255, 220, 0));
                HGDIOBJ op = SelectObject(dc, rp);
                HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
                Ellipse(dc, sx - 22, sy - 22, sx + 23, sy + 23);
                SelectObject(dc, ob);
                SelectObject(dc, op);
                DeleteObject(rp);
            }
            if (is_sel) {
                HPEN rp = CreatePen(PS_SOLID, 2, CLR_RING);
                HGDIOBJ op = SelectObject(dc, rp);
                HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
                Ellipse(dc, sx - 24, sy - 24, sx + 25, sy + 25);
                SelectObject(dc, ob);
                SelectObject(dc, op);
                DeleteObject(rp);
            }
            draw_plane(dc, sx, sy, g_ac[i].has_heading ? g_ac[i].heading : 0.0, is_sel ? 1.9 : 1.5, col,
                       is_sel ? RGB(255, 255, 255) : RGB(0, 0, 0));

            char label[96];
            if (is_sel && g_ac[i].alt[0]) {
                char at[48];
                alt_ft_text(g_ac[i].alt, at, sizeof(at), NULL);
                snprintf(label, sizeof(label), "%.23s  %.47s", g_ac[i].reg, at);
            } else
                snprintf(label, sizeof(label), "%.23s", g_ac[i].reg);
            WCHAR wl[96];
            u8_to_w(label, wl, 96);
            SIZE sz;
            GetTextExtentPoint32W(dc, wl, (int)wcslen(wl), &sz);
            RECT lr = {sx + 18, sy - 22, sx + 18 + sz.cx + 8, sy - 22 + sz.cy + 2};
            if (!is_sel && !is_hit && !label_free(&lr, lbl, nlbl))
                continue; /* подпись наехала бы на чужую - у самолёта остаётся цвет и номер в списке */
            if (nlbl < 400)
                lbl[nlbl++] = lr;
            HBRUSH lb = CreateSolidBrush(CLR_PANEL);
            FillRect(dc, &lr, lb);
            DeleteObject(lb);
            SetTextColor(dc, col);
            TextOutW(dc, lr.left + 4, lr.top + 1, wl, (int)wcslen(wl));
        }
        if (pass == 1 && g_search[0]) {
            char ov[160];
            if (g_search_cnt > 0)
                snprintf(ov, sizeof ov, "Поиск \"%.40s\": найдено бортов - %d%s", g_search, g_search_cnt,
                         g_search_cnt > 1 ? "  (Enter - следующий)" : "");
            else
                snprintf(ov, sizeof ov, "Поиск \"%.40s\": ничего не найдено среди бортов на карте", g_search);
            WCHAR wov[160];
            u8_to_w(ov, wov, 160);
            SIZE osz;
            HGDIOBJ of = SelectObject(dc, g_fontBold);
            GetTextExtentPoint32W(dc, wov, (int)wcslen(wov), &osz);
            RECT orc = {m->left + 10, m->top + 10, m->left + 10 + osz.cx + 16, m->top + 10 + osz.cy + 8};
            HBRUSH ob2 = CreateSolidBrush(CLR_PANEL);
            FillRect(dc, &orc, ob2);
            DeleteObject(ob2);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, g_search_cnt > 0 ? RGB(255, 220, 0) : RGB(255, 120, 120));
            TextOutW(dc, orc.left + 8, orc.top + 4, wov, (int)wcslen(wov));
            SelectObject(dc, of);
        }
        if (pass == 1 && sel >= 0 && g_show_trails)
            draw_track_points(dc, m, sel, plane_color(sel), lbl, &nlbl, 400);
        if (pass == 1 && sel >= 0 && g_sel_pt >= 0 && g_sel_pt < g_ac[sel].n_trail)
            draw_point_card(dc, m, &g_ac[sel], g_sel_pt, plane_color(sel));
    }

    /* подпись об авторстве подложки (обязательна по лицензии OSM) */
    SelectObject(dc, g_fontSmall);
    if (!kStyles[g_style].local) {
        const WCHAR *attr = kStyles[g_style].attribution;
        SIZE sz;
        GetTextExtentPoint32W(dc, attr, (int)wcslen(attr), &sz);
        RECT ar = {m->right - sz.cx - 12, m->bottom - sz.cy - 8, m->right, m->bottom};
        HBRUSH ab = CreateSolidBrush(CLR_PANEL);
        FillRect(dc, &ar, ab);
        DeleteObject(ab);
        SetTextColor(dc, CLR_DIM);
        TextOutW(dc, ar.left + 6, ar.top + 3, attr, (int)wcslen(attr));
    }

    /* для локального кэша - постоянная строка: какая папка, какая проекция, какие масштабы */
    if (kStyles[g_style].local) {
        const WCHAR *leaf = g_localdir[g_style];
        for (const WCHAR *q = g_localdir[g_style]; *q; q++)
            if (*q == L'\\' || *q == L'/')
                leaf = q + 1;
        WCHAR info[700], ovl[300];
        ovl[0] = L'\0';
        if (kStyles[g_style].has_overlay) {
            const WCHAR *l2 = g_overlaydir[g_style];
            for (const WCHAR *q = g_overlaydir[g_style]; *q; q++)
                if (*q == L'\\' || *q == L'/')
                    l2 = q + 1;
            if (g_overlaydir[g_style][0])
                wsprintfW(ovl, L"  |  слой: %s", l2);
            else
                wcscpy(ovl, L"  |  слой подписей не выбран");
        }
        if (g_show_info) {
            if (g_silver_count > 0) {
                WCHAR sv[100];
                wsprintfW(sv, L"  |  пустых серых тайлов в кэше: %d", g_silver_count);
                wcscat(ovl, sv);
            }
            WCHAR cv[80];
            if (g_view_pending > 0)
                wsprintfW(cv, L"  |  тайлов в кадре: %d из %d (грузится: %d)", g_view_have, g_view_total, g_view_pending);
            else
                wsprintfW(cv, L"  |  тайлов в кадре: %d из %d", g_view_have, g_view_total);
            wcscat(ovl, cv);
            wsprintfW(info, L"SAS.Planet: %s  |  %s  |  масштабы %d-%d  |  сейчас %d%s",
                      leaf, kStyles[g_style].proj == GEO_PROJ_ELLIPSOID ? L"EPSG:3395" : L"EPSG:3857",
                      g_zmin_s[g_style] + 1, g_zmax_s[g_style] + 1, g_zoom + 1, ovl);
        } else {
            /* обычный вид: только масштаб (в нумерации SAS.Planet - той же, что в папках z8, z9...) и,
             * если нужен слой подписей, а он не выбран, короткое предупреждение */
            wsprintfW(info, L"Масштаб %d%s", g_zoom + 1,
                      (kStyles[g_style].has_overlay && !g_overlaydir[g_style][0]) ? L"  |  слой подписей не выбран" : L"");
        }
        SelectObject(dc, g_fontSmall);
        SIZE isz;
        GetTextExtentPoint32W(dc, info, (int)wcslen(info), &isz);
        RECT ir = {m->left + 8, m->top + 6, m->left + 8 + isz.cx + 12, m->top + 6 + isz.cy + 6};
        HBRUSH ib = CreateSolidBrush(CLR_PANEL);
        FillRect(dc, &ir, ib);
        DeleteObject(ib);
        SetTextColor(dc, CLR_DIM);
        TextOutW(dc, ir.left + 6, ir.top + 3, info, (int)wcslen(info));

        /* вторая строка: какого именно тайла не хватает (путь внутри папки карты, расширение любое) */
        if (g_show_info && g_view_miss_have && kStyles[g_style].local) {
            WCHAR mis[200];
            wsprintfW(mis, L"Нет файла в кэше: z%d\\%d\\x%d\\%d\\y%d.*  (внутри папки карты; расширение любое)",
                      g_view_miss_z + 1, g_view_miss_x / 1024, g_view_miss_x, g_view_miss_y / 1024, g_view_miss_y);
            SIZE msz;
            GetTextExtentPoint32W(dc, mis, (int)wcslen(mis), &msz);
            RECT mr = {ir.left, ir.bottom + 2, ir.left + msz.cx + 12, ir.bottom + 2 + msz.cy + 6};
            HBRUSH mb = CreateSolidBrush(CLR_PANEL);
            FillRect(dc, &mr, mb);
            DeleteObject(mb);
            SetTextColor(dc, CLR_AMBER);
            TextOutW(dc, mr.left + 6, mr.top + 3, mis, (int)wcslen(mis));

            /* третья строка: габариты недостающего в координатах - их можно ввести в SAS.Planet для докачки */
            char area[420];
            WCHAR warea[420];
            format_missing_area(area, sizeof(area), g_view_miss_n, g_view_total, g_view_miss_z, g_view_miss_x0,
                                g_view_miss_x1, g_view_miss_y0, g_view_miss_y1, kStyles[g_style].proj, g_fill_mode);
            u8_to_w(area, warea, 420);
            SIZE asz;
            GetTextExtentPoint32W(dc, warea, (int)wcslen(warea), &asz);
            RECT ar = {mr.left, mr.bottom + 2, mr.left + asz.cx + 12, mr.bottom + 2 + asz.cy + 6};
            HBRUSH ab = CreateSolidBrush(CLR_PANEL);
            FillRect(dc, &ar, ab);
            DeleteObject(ab);
            TextOutW(dc, ar.left + 6, ar.top + 3, warea, (int)wcslen(warea));

            /* четвёртая строка: те же углы в градусах-минутах-секундах - можно вписывать в диалог SAS.Planet как есть */
            char dms[420];
            WCHAR wdms[420];
            format_missing_dms(dms, sizeof(dms), g_view_miss_z, g_view_miss_x0, g_view_miss_x1, g_view_miss_y0,
                               g_view_miss_y1, kStyles[g_style].proj);
            u8_to_w(dms, wdms, 420);
            SIZE dsz;
            GetTextExtentPoint32W(dc, wdms, (int)wcslen(wdms), &dsz);
            RECT dr = {ar.left, ar.bottom + 2, ar.left + dsz.cx + 12, ar.bottom + 2 + dsz.cy + 6};
            HBRUSH db = CreateSolidBrush(CLR_PANEL);
            FillRect(dc, &dr, db);
            DeleteObject(db);
            TextOutW(dc, dr.left + 6, dr.top + 3, wdms, (int)wcslen(wdms));
        }
    }

    /* состояние подложки - чтобы было понятно, почему нет карты */
    if (!drawn && g_view_pending == 0) {
        const char *msg;
        if (kStyles[g_style].local) {
            msg = NULL; /* вне скачанной области просто пусто, без сообщений */
        } else if (g_gdip_failed)
            msg = "Подложка недоступна: в системе не найдена gdiplus.dll - показана координатная сетка.";
        else if (g_net_status == 1)
            msg = "Подложка недоступна: не удалось подключить wininet.dll - показана координатная сетка.";
        else if (g_net_status == 2)
            msg = "Тайлы не загрузились (нет интернета?) - пока показана сетка.";
        else
            msg = "Загружаю тайлы карты...";
        if (msg) {
            SelectObject(dc, g_font);
            SetTextColor(dc, CLR_AMBER);
            put_text(dc, m->left + 10, m->bottom - 26, msg);
        }
    }
    RestoreDC(dc, -1);
}

/* ---------------------------------------------------------------- */
/* Фото борта: публичный Photo API сайта Planespotters.net            */
/* ---------------------------------------------------------------- */
/* Условия API (https://www.planespotters.net/photo/api), которые здесь выполняются:
 *  - рядом с фото видна подпись фотографа, а само фото - ссылка на страницу снимка
 *    (адрес из ответа API, без изменений);
 *  - запрос представляется описательным User-Agent с КОНТАКТОМ владельца программы
 *    (почта или сайт) - его вводит пользователь, придумывать его программа не вправе;
 *  - картинки НЕ сохраняются на диск и не публикуются: они скачиваются прямо с адреса из
 *    ответа API в память, показываются и выбрасываются (WinINet ещё и просим не писать
 *    их в свой кэш); ответ API хранится только в памяти и не дольше часа
 *    (разрешено до 24 часов);
 *  - запросы делаются только по щелчку на борт, по одному и без повторов в цикле. */
#define WM_APP_PHOTO        (WM_APP + 22)
#define PHOTO_SLOTS         8
#define PHOTO_TTL_MS        3600000UL
#define PHOTO_ERR_RETRY_MS  60000UL
#define PHOTO_PENDING_MS    30000UL
#define PHOTO_API_PREFIX    "https://api.planespotters.net/pub/photos/"
#define PHOTO_CDN_PREFIX    "https://cdn.planespotters.net/"
#define PHOTO_PAGE_PREFIX   "https://www.planespotters.net/photo/"

enum { PH_FREE = 0, PH_PENDING = 1, PH_READY = 2, PH_NONE = 3, PH_ERROR = 4, PH_OFF = 5 /* из интернета не запрашивалось: контакт не указан */,
       PH_BAD = 6 /* ваш файл найден, но не открывается как картинка */ };

typedef struct {
    char key[48]; /* хвост адреса API: "hex/155A21" или "reg/RA-73104" */
    int state;
    DWORD stamp;  /* когда получен ответ (или отправлен запрос) */
    unsigned use;
    HBITMAP bmp;
    int w, h;
    int local; /* 1 - это ваш собственный файл из папки aircraft_photos, 0 - фото из интернета */
    int fmt_ok; /* для PH_BAD: 1 - настоящий формат файла обычный (JPEG/PNG/...), но открыть не вышло */
    int rule;   /* для своего фото: по какому правилу найдено (1 - борт ... 6 - компания), см. photo_rule_match */
    WCHAR lname[200]; /* имя найденного файла */
    char photographer[100];
    char link[300];
} PhotoSlot;

typedef struct { /* результат фонового потока, передаётся окну целиком */
    char key[48];
    int status; /* PH_READY / PH_NONE / PH_ERROR */
    BYTE *bytes;
    DWORD nbytes;
    char photographer[100];
    char link[300];
} PhotoResult;

typedef struct {
    char image_url[300];
    char link[300];
    char photographer[100];
} PhotoMeta;

static PhotoSlot g_photos[PHOTO_SLOTS];
static unsigned g_photo_use;
static CRITICAL_SECTION g_pcs;
static HANDLE g_pevt, g_pworker;
static char g_preq[48]; /* ожидающий запрос (последний побеждает) */
static RECT g_photo_rc; /* где в панели нарисовано фото/подсказка - для кликов */
static int g_photo_click; /* 0 - нет, 1 - клик открывает страницу фото, 2 - клик запускает настройку */
static char g_photo_link[300];
static RECT g_folder_rc; /* область ссылки "Открыть папку aircraft_photos" под силуэтом */
static int g_folder_click;

static int starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* ---- разбор JSON ответа (минимальный: нужны всего три строки) ---- */
static int pj_hex4(const char *p, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else return 0;
        v = v * 16 + d;
    }
    *out = v;
    return 1;
}

static void pj_put_utf8(char *out, size_t cap, size_t *o, unsigned cp) {
    char t[4];
    int n;
    if (cp < 0x80) { t[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { t[0] = (char)(0xC0 | (cp >> 6)); t[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        t[0] = (char)(0xE0 | (cp >> 12)); t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        t[0] = (char)(0xF0 | (cp >> 18)); t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        t[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    }
    if (*o + (size_t)n >= cap)
        return; /* символ целиком не влезает - лучше обрезать строку, чем разорвать символ */
    for (int i = 0; i < n; i++)
        out[(*o)++] = t[i];
}

/* p - сразу за открывающей кавычкой строки JSON. Копирует значение с раскрытием
 * экранирования (\/ \" \\ \n \uXXXX, в том числе пары суррогатов) в out как UTF-8.
 * Возвращает указатель за закрывающей кавычкой или NULL, если строка оборвана/испорчена. */
static const char *pj_string_at(const char *p, char *out, size_t cap) {
    size_t o = 0;
    while (*p && *p != '"') {
        if (*p != '\\') {
            if (o + 1 < cap)
                out[o++] = *p;
            p++;
            continue;
        }
        p++;
        unsigned cp;
        switch (*p) {
        case 'n': cp = '\n'; p++; break;
        case 't': cp = '\t'; p++; break;
        case 'r': cp = '\r'; p++; break;
        case 'b': cp = '\b'; p++; break;
        case 'f': cp = '\f'; p++; break;
        case 'u': {
            unsigned u;
            if (!pj_hex4(p + 1, &u)) {
                out[o] = '\0';
                return NULL;
            }
            p += 5;
            cp = u;
            if (u >= 0xD800 && u < 0xDC00 && p[0] == '\\' && p[1] == 'u') {
                unsigned lo;
                if (pj_hex4(p + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            break;
        }
        case '\0':
            out[o] = '\0';
            return NULL;
        default: /* \/ \" \\ и прочие - сам символ */
            cp = (unsigned char)*p;
            p++;
            break;
        }
        pj_put_utf8(out, cap, &o, cp);
    }
    out[o] = '\0';
    return *p == '"' ? p + 1 : NULL;
}

static int pj_is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int pj_is_word(char c) { return isalnum((unsigned char)c) || c == '_'; }

/* Находит ключ name и возвращает указатель сразу за ним (NULL, если нет). Ключ может быть и в
 * кавычках (настоящий JSON), и без (так ответ показан на странице документации API) - нам
 * нельзя полагаться на то, какой из видов придёт: проверить это на живом ответе отсюда нельзя.
 * Без кавычек ключ засчитывается, только если за ним идёт двоеточие и перед ним не буква/цифра,
 * чтобы не принять за ключ часть другого слова или текст внутри значения. */
static const char *pj_find_key(const char *from, const char *name) {
    size_t n = strlen(name);
    for (const char *p = from; *p; p++) {
        if (*p == '"' && strncmp(p + 1, name, n) == 0 && p[1 + n] == '"')
            return p + n + 2;
        if (strncmp(p, name, n) == 0 && (p == from || (!pj_is_word(p[-1]) && p[-1] != '"')) &&
            !pj_is_word(p[n]) && p[n] != '"') {
            const char *q = p + n;
            while (pj_is_ws(*q))
                q++;
            if (*q == ':')
                return p + n;
        }
    }
    return NULL;
}

/* Ищет  name : "значение"  начиная с from; значение расэкранируется в out. Возвращает указатель
 * за значением или NULL. */
static const char *pj_get(const char *from, const char *name, char *out, size_t cap) {
    const char *p = from;
    while ((p = pj_find_key(p, name)) != NULL) {
        const char *q = p;
        while (pj_is_ws(*q))
            q++;
        if (*q == ':') {
            q++;
            while (pj_is_ws(*q))
                q++;
            if (*q == '"')
                return pj_string_at(q + 1, out, cap);
        }
    }
    return NULL;
}

/* 1 - фото есть (meta заполнена), 0 - фото нет ("photos":[]), -1 - ошибка или ответ, которому
 * нельзя доверять (нет обязательных полей, адреса не с сайтов Planespotters). Фото без имени
 * фотографа не показывается: условия API требуют подпись. */
static int photo_parse_response(const char *json, PhotoMeta *m) {
    memset(m, 0, sizeof(*m));
    const char *q = pj_find_key(json, "photos");
    if (!q)
        return -1;
    while (pj_is_ws(*q)) q++;
    if (*q != ':')
        return -1;
    q++;
    while (pj_is_ws(*q)) q++;
    if (*q != '[')
        return -1;
    q++;
    while (pj_is_ws(*q)) q++;
    if (*q == ']')
        return 0;
    if (*q == '\0')
        return -1;

    const char *lg = pj_find_key(q, "thumbnail_large");
    const char *th = pj_find_key(q, "thumbnail");
    int got = 0;
    if (lg && pj_get(lg, "src", m->image_url, sizeof(m->image_url)))
        got = 1;
    else if (th && pj_get(th, "src", m->image_url, sizeof(m->image_url)))
        got = 1;
    if (!got)
        return -1;
    if (!pj_get(q, "link", m->link, sizeof(m->link)))
        return -1;
    if (!pj_get(q, "photographer", m->photographer, sizeof(m->photographer)) || !m->photographer[0])
        return -1;
    if (!starts_with(m->image_url, PHOTO_CDN_PREFIX) || !starts_with(m->link, PHOTO_PAGE_PREFIX))
        return -1;
    return 1;
}

/* ---- ключ запроса ---- */
/* В 7-символьное поле адреса ACARS дефис не влезает у регистраций из 8 знаков: у России
 * "RA-73104" приходит как "RA73104". Для запроса по регистрации дефис нужно вернуть. */
static void photo_reg_normalize(const char *reg, char *out, size_t cap) {
    char t[24];
    size_t n = 0;
    for (; *reg && n + 1 < sizeof(t); reg++) {
        char c = *reg;
        if (c == '.' || c == ' ')
            continue;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        t[n++] = c;
    }
    t[n] = '\0';
    if (n == 7 && strchr(t, '-') == NULL) {
        static const char *const pfx[] = {"RA", "RF", "UR", "UK", "EW", "UP", "EX", "EY", "EZ", "ER", "4K", "4L"};
        for (size_t i = 0; i < sizeof(pfx) / sizeof(pfx[0]); i++) {
            if (t[0] == pfx[i][0] && t[1] == pfx[i][1]) {
                snprintf(out, cap, "%c%c-%.5s", t[0], t[1], t + 2); /* здесь знаков ровно 7: 2 + 5 */
                return;
            }
        }
    }
    snprintf(out, cap, "%s", t);
}

/* Ключ идёт прямо в адрес запроса, поэтому проверяется строго. */
static int photo_key_valid(const char *key) {
    if (strncmp(key, "hex/", 4) == 0) {
        const char *h = key + 4;
        if (strlen(h) != 6)
            return 0;
        for (int i = 0; i < 6; i++)
            if (!isxdigit((unsigned char)h[i]))
                return 0;
        return 1;
    }
    if (strncmp(key, "reg/", 4) == 0) {
        const char *r = key + 4;
        size_t n = strlen(r);
        if (n < 2 || n > 12)
            return 0;
        for (size_t i = 0; i < n; i++) {
            char c = r[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'))
                return 0;
        }
        return 1;
    }
    return 0;
}

/* По коду Mode S, если он известен: тогда API отдаёт фото именно этого экземпляра самолёта
 * (регистрации со временем переходят от борта к борту). Иначе - по регистрации. */
static int photo_make_key(const MapAircraft *a, char *key, size_t cap) {
    int hexok = strlen(a->hex) == 6;
    for (int i = 0; hexok && i < 6; i++)
        if (!isxdigit((unsigned char)a->hex[i]))
            hexok = 0;
    char tmp[48];
    if (hexok) {
        char h[7];
        for (int i = 0; i < 6; i++)
            h[i] = (char)toupper((unsigned char)a->hex[i]);
        h[6] = '\0';
        snprintf(tmp, sizeof(tmp), "hex/%s", h);
    } else {
        char norm[24];
        photo_reg_normalize(a->reg, norm, sizeof(norm));
        snprintf(tmp, sizeof(tmp), "reg/%s", norm);
    }
    if (!photo_key_valid(tmp))
        return 0;
    snprintf(key, cap, "%s", tmp);
    return 1;
}

/* ---- контакт владельца для User-Agent ---- */
static int contact_valid(const char *c) {
    size_t n = strlen(c);
    return n >= 5 && n <= 120 && (strchr(c, '@') != NULL || strchr(c, '.') != NULL);
}

static int read_contact(char *out, size_t cap) {
    WCHAR ini[MAX_PATH + 32], w[200];
    cwd_file(L"acars_settings.ini", ini);
    w[0] = L'\0';
    GetPrivateProfileStringW(L"Photos", L"Contact", L"", w, 200, ini);
    size_t o = 0;
    for (const WCHAR *p = w; *p && o + 1 < cap; p++)
        if (*p > 0x20 && *p < 0x7F && *p != L'(' && *p != L')') /* только печатный ASCII без скобок */
            out[o++] = (char)*p;
    out[o] = '\0';
    return contact_valid(out);
}

/* Для отрисовки: проверка контакта не чаще раза в полторы секунды (иначе файл настроек
 * читался бы при каждой перерисовке, а их при перетаскивании десятки в секунду). */
static int contact_ok_cached(void) {
    static int ok, have;
    static DWORD stamp;
    DWORD now = GetTickCount();
    if (!have || now - stamp > 1500) {
        char c[160];
        ok = read_contact(c, sizeof(c));
        stamp = now;
        have = 1;
    }
    return ok;
}

/* ---- сеть (в фоновом потоке) ---- */
typedef struct {
    PFN_InternetOpenW open;
    PFN_InternetOpenUrlW openurl;
    PFN_InternetReadFile read;
    PFN_InternetCloseHandle close;
    PFN_InternetSetOptionW setopt;
} MvNet;

/* Скачивает адрес целиком в память (не больше cap байт; в конец добавляется ноль). NULL при
 * неудаче. INTERNET_FLAG_NO_CACHE_WRITE - чтобы WinINet не оставил копию в своём кэше на диске. */
static BYTE *net_get(const MvNet *n, MvInet session, const char *url_utf8, DWORD cap, DWORD *len) {
    WCHAR wurl[420];
    if (MultiByteToWideChar(CP_UTF8, 0, url_utf8, -1, wurl, 420) == 0)
        return NULL;
    MvInet u = n->openurl(session, wurl, NULL, 0,
                          MV_INTERNET_FLAG_RELOAD | MV_INTERNET_FLAG_NO_CACHE_WRITE |
                              MV_INTERNET_FLAG_NO_UI | MV_INTERNET_FLAG_SECURE,
                          0);
    if (!u)
        return NULL;
    BYTE *buf = (BYTE *)malloc((size_t)cap + 1);
    DWORD total = 0;
    if (buf) {
        for (;;) {
            DWORD got = 0;
            if (!n->read(u, buf + total, cap - total, &got) || got == 0)
                break;
            total += got;
            if (total >= cap)
                break;
        }
        buf[total] = 0;
    }
    n->close(u);
    if (!buf)
        return NULL;
    if (total == 0) {
        free(buf);
        return NULL;
    }
    *len = total;
    return buf;
}

static void photo_fetch(const MvNet *n, const char *key, PhotoResult *r) {
    r->status = PH_ERROR;
    char contact[160];
    if (!photo_key_valid(key) || !read_contact(contact, sizeof(contact)))
        return;
    char ua[260];
    snprintf(ua, sizeof(ua), "AcarsDecoder/1.0 (+%s)", contact); /* представляемся, как требуют условия API */
    WCHAR wua[260];
    if (MultiByteToWideChar(CP_UTF8, 0, ua, -1, wua, 260) == 0)
        return;
    MvInet session = n->open(wua, 0 /* INTERNET_OPEN_TYPE_PRECONFIG */, NULL, NULL, 0);
    if (!session)
        return;
    if (n->setopt) {
        DWORD ms = 10000;
        n->setopt(session, MV_INTERNET_OPTION_CONNECT_TIMEOUT, &ms, sizeof(ms));
        n->setopt(session, MV_INTERNET_OPTION_SEND_TIMEOUT, &ms, sizeof(ms));
        n->setopt(session, MV_INTERNET_OPTION_RECEIVE_TIMEOUT, &ms, sizeof(ms));
    }

    char url[160];
    snprintf(url, sizeof(url), PHOTO_API_PREFIX "%s", key);
    DWORD jl = 0;
    BYTE *json = net_get(n, session, url, 65536, &jl);
    if (json) {
        PhotoMeta m;
        int pr = photo_parse_response((const char *)json, &m);
        if (pr == 0) {
            r->status = PH_NONE;
        } else if (pr > 0) {
            DWORD il = 0;
            BYTE *img = net_get(n, session, m.image_url, 1048576, &il); /* адрес - ровно из ответа API */
            int jpeg = img && il > 100 && img[0] == 0xFF && img[1] == 0xD8;
            int png = img && il > 100 && img[0] == 0x89 && img[1] == 'P';
            if (jpeg || png) {
                r->bytes = img;
                r->nbytes = il;
                img = NULL;
                r->status = PH_READY;
                snprintf(r->photographer, sizeof(r->photographer), "%s", m.photographer);
                snprintf(r->link, sizeof(r->link), "%s", m.link);
            }
            free(img);
        }
        free(json);
    }
    n->close(session);
}

static DWORD WINAPI photo_worker(LPVOID param) {
    (void)param;
    MvNet n;
    memset(&n, 0, sizeof(n));
    HMODULE hw = LoadLibraryW(L"wininet.dll");
    if (hw) {
        n.open = (PFN_InternetOpenW)(void *)GetProcAddress(hw, "InternetOpenW");
        n.openurl = (PFN_InternetOpenUrlW)(void *)GetProcAddress(hw, "InternetOpenUrlW");
        n.read = (PFN_InternetReadFile)(void *)GetProcAddress(hw, "InternetReadFile");
        n.close = (PFN_InternetCloseHandle)(void *)GetProcAddress(hw, "InternetCloseHandle");
        n.setopt = (PFN_InternetSetOptionW)(void *)GetProcAddress(hw, "InternetSetOptionW");
    }
    int net_ok = n.open && n.openurl && n.read && n.close;

    for (;;) {
        WaitForSingleObject(g_pevt, INFINITE);
        for (;;) {
            char key[48];
            EnterCriticalSection(&g_pcs);
            snprintf(key, sizeof(key), "%s", g_preq);
            g_preq[0] = '\0';
            LeaveCriticalSection(&g_pcs);
            if (!key[0])
                break;
            PhotoResult *r = (PhotoResult *)calloc(1, sizeof(PhotoResult));
            if (!r)
                continue;
            snprintf(r->key, sizeof(r->key), "%s", key);
            r->status = PH_ERROR;
            if (net_ok)
                photo_fetch(&n, key, r);
            if (!g_hwndMap || !PostMessageW(g_hwndMap, WM_APP_PHOTO, 0, (LPARAM)r)) {
                free(r->bytes);
                free(r);
            }
        }
    }
    return 0;
}

/* ---- декодирование картинки прямо из памяти (на диск не пишется) ---- */
typedef LONG (WINAPI *PFN_CreateStreamOnHGlobal)(HANDLE, BOOL, void **);
typedef ULONG (WINAPI *PFN_ComRelease)(void *);

static HBITMAP decode_image_bytes(const BYTE *data, DWORD n, int *w, int *h) {
    if (!gdip_init() || !pGdipCreateBitmapFromStream)
        return NULL;
    HMODULE ole = LoadLibraryW(L"ole32.dll");
    PFN_CreateStreamOnHGlobal pCreate =
        ole ? (PFN_CreateStreamOnHGlobal)(void *)GetProcAddress(ole, "CreateStreamOnHGlobal") : NULL;
    if (!pCreate)
        return NULL;
    HANDLE hg = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!hg)
        return NULL;
    void *mem = GlobalLock(hg);
    if (!mem) {
        GlobalFree(hg);
        return NULL;
    }
    memcpy(mem, data, n);
    GlobalUnlock(hg);

    void *stream = NULL;
    if (pCreate(hg, TRUE, &stream) != 0 || !stream) { /* TRUE: память освободится вместе с потоком */
        GlobalFree(hg);
        return NULL;
    }
    HBITMAP hb = NULL;
    void *bmp = NULL;
    if (pGdipCreateBitmapFromStream(stream, &bmp) == 0 && bmp) {
        if (pGdipCreateHBITMAPFromBitmap(bmp, &hb, 0) != 0)
            hb = NULL;
        pGdipDisposeImage(bmp); /* пиксели уже скопированы в HBITMAP - поток можно отпускать */
    }
    /* IStream: в таблице методов Release - третий (после QueryInterface и AddRef) */
    ((PFN_ComRelease)(*(void ***)stream)[2])(stream);

    if (hb) {
        BITMAP bm;
        if (GetObjectW(hb, sizeof(bm), &bm) != 0 && bm.bmWidth > 0 && bm.bmHeight > 0) {
            *w = (int)bm.bmWidth;
            *h = (int)bm.bmHeight;
        } else {
            DeleteObject(hb);
            hb = NULL;
        }
    }
    return hb;
}

/* ---- ячейки результатов и запрос ---- */
static PhotoSlot *photo_find(const char *key) {
    for (int i = 0; i < PHOTO_SLOTS; i++)
        if (g_photos[i].state != PH_FREE && strcmp(g_photos[i].key, key) == 0)
            return &g_photos[i];
    return NULL;
}

static PhotoSlot *photo_alloc(const char *key) {
    int pick = -1;
    unsigned best = 0xFFFFFFFFu;
    for (int i = 0; i < PHOTO_SLOTS; i++) {
        if (g_photos[i].state == PH_FREE) {
            pick = i;
            break;
        }
    }
    if (pick < 0) {
        for (int i = 0; i < PHOTO_SLOTS; i++) {
            if (g_photos[i].state != PH_PENDING && g_photos[i].use <= best) {
                best = g_photos[i].use;
                pick = i;
            }
        }
    }
    if (pick < 0)
        pick = 0;
    PhotoSlot *s = &g_photos[pick];
    if (s->bmp)
        DeleteObject(s->bmp);
    memset(s, 0, sizeof(*s));
    snprintf(s->key, sizeof(s->key), "%s", key);
    s->use = ++g_photo_use;
    return s;
}

/* ---- собственная папка фото: работает без интернета ---- */
/* Папка создаётся самой программой (рядом с acars_logs - в рабочей папке), чтобы её не нужно было
 * делать вручную; "Открыть папку" под силуэтом открывает её в проводнике. */
static void ensure_photo_dir(void) {
    WCHAR d[MAX_PATH + 32];
    cwd_file(L"aircraft_photos", d);
    CreateDirectoryW(d, NULL); /* если уже есть - ничего не происходит */
}

static void open_photo_dir(void) {
    WCHAR d[MAX_PATH + 32];
    ensure_photo_dir();
    cwd_file(L"aircraft_photos", d);
    ShellExecuteW(NULL, L"open", d, NULL, NULL, SW_SHOWNORMAL);
}

/* Какие расширения считаем картинками. GDI+ читает jpg/png/gif/bmp/tif; webp, avif, heic, heif
 * пробуем открыть средствами Windows (WIC) - если нужный кодек в системе есть. */
static const WCHAR *const LOCAL_EXTS[] = {L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".gif", L".bmp",
                                          L".tif", L".tiff", L".webp", L".avif", L".heic", L".heif"};
#define N_LOCAL_EXTS ((int)(sizeof(LOCAL_EXTS) / sizeof(LOCAL_EXTS[0])))

#define PHOTO_FILES_MAX 3000
typedef struct {
    WCHAR name[200]; /* имя файла как есть */
    WCHAR k1[100];   /* ключ имени: без расширения(й), без " (N)", только заглавные буквы и цифры */
    WCHAR k2[100];   /* то же, но ещё без хвоста вида "_2"/"-2" - вторая попытка */
} PhotoFile;

typedef struct { /* ключи самого борта, в том же виде, что у имён файлов */
    WCHAR reg[24];
    WCHAR hex[10];
    WCHAR type[24];
    WCHAR model[48];
    WCHAR op[100];
} PhotoKeys;

/* Ключ сравнения: только буквы и цифры, регистр не важен. "RA-73764", "ra73764", "RA 73764" и
 * "A320_214" / "A320-214" / "A320 214" этим сводятся к одному виду. Работает и с кириллицей. */
static void photo_norm(const WCHAR *src, WCHAR *out, int cap) {
    int o = 0;
    for (; *src && o + 1 < cap; src++)
        if (iswalnum((wint_t)*src))
            out[o++] = (WCHAR)towupper((wint_t)*src);
    out[o] = L'\0';
}

static int strip_image_ext(WCHAR *s) {
    WCHAR *dot = wcsrchr(s, L'.');
    if (!dot)
        return 0;
    for (int i = 0; i < N_LOCAL_EXTS; i++)
        if (_wcsicmp(dot, LOCAL_EXTS[i]) == 0) {
            *dot = L'\0';
            return 1;
        }
    return 0;
}

/* Из имени файла делает ключи. 0 - это не картинка (расширение не из списка). Снимает: расширение,
 * повторное расширение ("RA-1.jpg.jpg" - Windows прячет расширение, и его вписывают второй раз) и
 * хвост " (1)", который Windows дописывает копиям. */
static int photo_file_keys(const WCHAR *fname, PhotoFile *pf) {
    WCHAR t[200];
    wcsncpy(t, fname, 199);
    t[199] = L'\0';
    if (!strip_image_ext(t))
        return 0;
    while (strip_image_ext(t)) {
    }
    size_t n = wcslen(t);
    if (n > 3 && t[n - 1] == L')') {
        size_t i = n - 1;
        while (i > 0 && t[i - 1] >= L'0' && t[i - 1] <= L'9')
            i--;
        if (i < n - 1 && i >= 2 && t[i - 1] == L'(') {
            size_t cut = i - 1;
            while (cut > 0 && t[cut - 1] == L' ')
                cut--;
            t[cut] = L'\0';
        }
    }
    wcsncpy(pf->name, fname, 199);
    pf->name[199] = L'\0';
    photo_norm(t, pf->k1, 100);
    WCHAR t2[200];
    wcscpy(t2, t);
    size_t m = wcslen(t2), e = m;
    while (e > 0 && t2[e - 1] >= L'0' && t2[e - 1] <= L'9')
        e--;
    if (e < m && e > 0 && (t2[e - 1] == L'_' || t2[e - 1] == L'-' || t2[e - 1] == L' ' || t2[e - 1] == L'.'))
        t2[e - 1] = L'\0';
    photo_norm(t2, pf->k2, 100);
    return pf->k1[0] != L'\0';
}

static void photo_keys_from(const MapAircraft *a, PhotoKeys *k) {
    WCHAR w[200];
    char nr[24];
    memset(k, 0, sizeof(*k));
    photo_reg_normalize(a->reg, nr, sizeof(nr));
    u8_to_w(nr, w, 200);
    photo_norm(w, k->reg, 24);
    u8_to_w(a->hex, w, 200);
    photo_norm(w, k->hex, 10);
    u8_to_w(a->type, w, 200);
    photo_norm(w, k->type, 24);
    u8_to_w(a->model, w, 200);
    photo_norm(w, k->model, 48);
    u8_to_w(a->operator_name, w, 200);
    photo_norm(w, k->op, 100);
}

/* Файл "<основа><компания>": ключ начинается с основы (тип или модель, не короче 3 знаков), а остаток
 * (не короче 3 знаков) - начало названия компании борта. Так файл "A320_Aeroflot" подходит к борту
 * компании "Aeroflot Russian Airlines", а "A320_Rossiya" - нет. */
static int key_prefix_op(const WCHAR *fk, const WCHAR *base, const WCHAR *op) {
    size_t bl = wcslen(base);
    if (bl < 3 || !op[0] || wcsncmp(fk, base, bl) != 0)
        return 0;
    const WCHAR *rest = fk + bl;
    size_t rl = wcslen(rest);
    return rl >= 3 && wcsncmp(op, rest, rl) == 0;
}

/* Правила по убыванию точности; побеждает первое, по которому нашёлся файл:
 *  1 - этот борт (регистрация или код Mode S)
 *  2 - модель + компания     (A320-214_Aeroflot)
 *  3 - тип + компания        (A320_Aeroflot)
 *  4 - модель                (A320-214)
 *  5 - тип                   (A320)
 *  6 - компания              (Aeroflot) */
static int photo_rule_match(int rule, const PhotoKeys *k, const WCHAR *fk) {
    size_t n = wcslen(fk);
    switch (rule) {
    case 1: return (k->reg[0] && wcscmp(fk, k->reg) == 0) || (k->hex[0] && wcscmp(fk, k->hex) == 0);
    case 2: return key_prefix_op(fk, k->model, k->op);
    case 3: return key_prefix_op(fk, k->type, k->op);
    case 4: return wcslen(k->model) >= 3 && wcscmp(fk, k->model) == 0;
    case 5: return wcslen(k->type) >= 3 && wcscmp(fk, k->type) == 0;
    case 6: return k->op[0] && n >= 4 && wcsncmp(k->op, fk, n) == 0;
    }
    return 0;
}

/* Читает список файлов-картинок папки dir (картинки по расширению) вместе с ключами. NULL - папки нет. */
static PhotoFile *photo_scan(const WCHAR *dir, int *count) {
    *count = 0;
    PhotoFile *arr = (PhotoFile *)malloc(sizeof(PhotoFile) * PHOTO_FILES_MAX);
    if (!arr)
        return NULL;
    WCHAR pat[MAX_PATH + 40];
    wsprintfW(pat, L"%s\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        free(arr);
        return NULL;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (*count >= PHOTO_FILES_MAX)
            break;
        if (photo_file_keys(fd.cFileName, &arr[*count]))
            (*count)++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return arr;
}

/* Ищет в <рабочая папка>\aircraft_photos фото для борта по правилам выше. 1 - найдено (полный путь -
 * out_path, правило - out_rule, имя файла - out_name). Внутри правила сначала точное совпадение
 * имени, затем без хвоста "_2". */
static int find_local_photo(const MapAircraft *a, WCHAR *out_path, int *out_rule, WCHAR *out_name) {
    WCHAR dir[MAX_PATH + 32];
    cwd_file(L"aircraft_photos", dir);
    int n = 0;
    PhotoFile *files = photo_scan(dir, &n);
    if (!files)
        return 0;
    PhotoKeys k;
    photo_keys_from(a, &k);
    int found = 0;
    for (int rule = 1; rule <= 6 && !found; rule++) {
        for (int pass = 0; pass < 2 && !found; pass++) {
            for (int i = 0; i < n; i++) {
                const WCHAR *fk = pass ? files[i].k2 : files[i].k1;
                if (fk[0] && photo_rule_match(rule, &k, fk)) {
                    wsprintfW(out_path, L"%s\\%s", dir, files[i].name);
                    wcsncpy(out_name, files[i].name, 199);
                    out_name[199] = L'\0';
                    *out_rule = rule;
                    found = 1;
                    break;
                }
            }
        }
    }
    free(files);
    return found;
}

/* Добавляет в строку подсказки имя "a" или "a_b" через разделитель " · ". */
static void hint_add(WCHAR *out, int cap, const WCHAR *a, const WCHAR *b) {
    size_t len = wcslen(out);
    size_t need = wcslen(a) + (b ? wcslen(b) + 1 : 0) + 4;
    if (len + need >= (size_t)cap)
        return;
    if (len)
        wcscat(out, L" \u00B7 ");
    wcscat(out, a);
    if (b) {
        wcscat(out, L"_");
        wcscat(out, b);
    }
}

/* Подсказка: под какими именами программа найдёт фото ЭТОГО борта (по убыванию точности). */
static void photo_hint_names(const MapAircraft *a, WCHAR *out, int cap) {
    WCHAR reg[40], model[60], type[40], opw[100], opfirst[100];
    char nr[24];
    out[0] = L'\0';
    photo_reg_normalize(a->reg, nr, sizeof(nr));
    u8_to_w(nr, reg, 40);
    u8_to_w(a->model, model, 60);
    u8_to_w(a->type, type, 40);
    u8_to_w(a->operator_name, opw, 100);
    size_t o = 0;
    while (opw[o] && opw[o] != L' ' && o < 99) { /* из названия компании берём первое слово */
        opfirst[o] = opw[o];
        o++;
    }
    opfirst[o] = L'\0';
    if (reg[0])
        hint_add(out, cap, reg, NULL);
    if (model[0] && opfirst[0])
        hint_add(out, cap, model, opfirst);
    if (type[0] && opfirst[0] && wcscmp(type, model) != 0)
        hint_add(out, cap, type, opfirst);
    if (model[0])
        hint_add(out, cap, model, NULL);
    if (type[0] && wcscmp(type, model) != 0)
        hint_add(out, cap, type, NULL);
    if (opfirst[0])
        hint_add(out, cap, opfirst, NULL);
}

/* ---- запасное чтение картинки средствами Windows (WIC) ---- */
/* Приложение "Фотографии" открывает WebP/AVIF/HEIC через WIC и установленные кодеки (на Windows 10 это
 * "Расширения для изображений WebP/HEIF" из Microsoft Store, обычно уже стоят). Тот же механизм
 * использует и карта - но только когда GDI+ файл открыть не смогла. Идентификаторы сверены с тремя
 * независимыми источниками, вызовы идут по именам методов из настоящего wincodec.h, а не по номерам. */
static const GUID MV_CLSID_WICFactory = {0xCACAF262, 0x9370, 0x4615, {0xA1, 0x3B, 0x9F, 0x55, 0x39, 0xDA, 0x4C, 0x0A}};
static const GUID MV_IID_WICFactory = {0xEC5EC8A9, 0xC395, 0x4314, {0x9C, 0x77, 0x54, 0xD7, 0xA9, 0x35, 0xFF, 0x70}};
static const GUID MV_PF_BGRA = {0x6FDDC324, 0x4E03, 0x4BFE, {0xB1, 0x85, 0x3D, 0x77, 0x76, 0x8D, 0xC9, 0x0F}};

#ifdef MV_HAVE_WIC
typedef LONG (WINAPI *PFN_CoCreateInstance)(const GUID *, void *, DWORD, const GUID *, void **);
typedef LONG (WINAPI *PFN_CoInitializeEx)(void *, DWORD);

/* Открывает файл через WIC, приводит к 32-битным BGRA и (если шире 1000 пикселей) сжимает. NULL при любой
 * неудаче: нет кодека, повреждённый файл, не хватило памяти. */
static HBITMAP wic_decode_file(const WCHAR *path, int *w, int *h) {
    static int com_done;
    HMODULE ole = LoadLibraryW(L"ole32.dll");
    if (!ole)
        return NULL;
    PFN_CoInitializeEx pInit = (PFN_CoInitializeEx)(void *)GetProcAddress(ole, "CoInitializeEx");
    PFN_CoCreateInstance pCreate = (PFN_CoCreateInstance)(void *)GetProcAddress(ole, "CoCreateInstance");
    if (!pCreate)
        return NULL;
    if (!com_done && pInit) {
        pInit(NULL, 2 /* COINIT_APARTMENTTHREADED */); /* результат не важен: если COM уже запущен иначе, он всё равно пригоден */
        com_done = 1;
    }

    IWICImagingFactory *factory = NULL;
    if (pCreate(&MV_CLSID_WICFactory, NULL, 1 /* CLSCTX_INPROC_SERVER */, &MV_IID_WICFactory, (void **)&factory) < 0 ||
        !factory)
        return NULL;

    HBITMAP result = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    IWICBitmapScaler *scaler = NULL;
    UINT iw = 0, ih = 0;

    if (factory->lpVtbl->CreateDecoderFromFilename(factory, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                   &decoder) >= 0 && decoder &&
        decoder->lpVtbl->GetFrame(decoder, 0, &frame) >= 0 && frame &&
        factory->lpVtbl->CreateFormatConverter(factory, &conv) >= 0 && conv &&
        conv->lpVtbl->Initialize(conv, (IWICBitmapSource *)frame, &MV_PF_BGRA, WICBitmapDitherTypeNone, NULL, 0.0,
                                 WICBitmapPaletteTypeCustom) >= 0 &&
        conv->lpVtbl->GetSize(conv, &iw, &ih) >= 0 && iw > 0 && ih > 0) {
        IWICBitmapSource *src = (IWICBitmapSource *)conv;
        UINT ow = iw, oh = ih;
        if (iw > 1000) {
            ow = 1000;
            oh = (UINT)((double)ih * ow / iw);
            if (oh < 1)
                oh = 1;
            if (factory->lpVtbl->CreateBitmapScaler(factory, &scaler) >= 0 && scaler &&
                scaler->lpVtbl->Initialize(scaler, (IWICBitmapSource *)conv, ow, oh, WICBitmapInterpolationModeFant) >= 0) {
                src = (IWICBitmapSource *)scaler;
            } else { /* сжать не вышло - читаем как есть */
                ow = iw;
                oh = ih;
            }
        }
        BITMAPINFO bmi;
        memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = (LONG)ow;
        bmi.bmiHeader.biHeight = -(LONG)oh; /* строки сверху вниз */
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void *bits = NULL;
        HBITMAP hb = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (hb && bits) {
            UINT stride = ow * 4;
            if (src->lpVtbl->CopyPixels(src, NULL, stride, stride * oh, (BYTE *)bits) >= 0) {
                result = hb;
                *w = (int)ow;
                *h = (int)oh;
            } else {
                DeleteObject(hb);
            }
        } else if (hb) {
            DeleteObject(hb);
        }
    }
    if (scaler) scaler->lpVtbl->Release(scaler);
    if (conv) conv->lpVtbl->Release(conv);
    if (frame) frame->lpVtbl->Release(frame);
    if (decoder) decoder->lpVtbl->Release(decoder);
    factory->lpVtbl->Release(factory);
    return result;
}
#else
static HBITMAP wic_decode_file(const WCHAR *path, int *w, int *h) {
    (void)path; (void)w; (void)h;
    return NULL; /* заголовка wincodec.h в этой сборке MinGW нет */
}
#endif

/* Загружает файл и, если он больше 1000 пикселей в ширину (фото с камеры - тысячи), сжимает: иначе
 * несколько таких снимков в памяти заняли бы сотни мегабайт, а панель всё равно узкая. */
static HBITMAP load_local_bitmap(const WCHAR *path, int *w, int *h) {
    HBITMAP hb = load_png_as_hbitmap(path); /* GDI+ определяет формат сама: jpg, png, gif, bmp, tif */
    if (!hb)
        return wic_decode_file(path, w, h); /* GDI+ не смогла (WebP, AVIF, HEIC...) - пробуем средствами Windows */
    BITMAP bm;
    if (GetObjectW(hb, sizeof(bm), &bm) == 0 || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        DeleteObject(hb);
        return NULL;
    }
    int bw = (int)bm.bmWidth, bh = (int)bm.bmHeight;
    const int maxw = 1000;
    if (bw > maxw) {
        int nw = maxw, nh = (int)((double)bh * nw / bw);
        if (nh < 1)
            nh = 1;
        HDC sdc = GetDC(NULL);
        if (sdc) {
            HDC m1 = CreateCompatibleDC(sdc), m2 = CreateCompatibleDC(sdc);
            HBITMAP nb = CreateCompatibleBitmap(sdc, nw, nh);
            if (nb && m1 && m2) {
                HGDIOBJ o1 = SelectObject(m1, hb), o2 = SelectObject(m2, nb);
                SetStretchBltMode(m2, HALFTONE);
                SetBrushOrgEx(m2, 0, 0, NULL);
                StretchBlt(m2, 0, 0, nw, nh, m1, 0, 0, bw, bh, SRCCOPY);
                SelectObject(m1, o1);
                SelectObject(m2, o2);
                DeleteObject(hb);
                hb = nb;
                bw = nw;
                bh = nh;
            } else if (nb) {
                DeleteObject(nb);
            }
            if (m1) DeleteDC(m1);
            if (m2) DeleteDC(m2);
            ReleaseDC(NULL, sdc);
        }
    }
    *w = bw;
    *h = bh;
    return hb;
}

/* Настоящий формат файла по его первым байтам. Расширение может врать: браузеры часто сохраняют
 * WebP или AVIF под именем .jpg, а iPhone отдаёт HEIC. Приложение "Фотографии" такое открывает,
 * а GDI+ (на ней работает карта) - нет. supported = 1, если GDI+ этот формат читает. */
static const char *sniff_image_format(const BYTE *b, size_t n, int *supported) {
    *supported = 0;
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) { *supported = 1; return "JPEG"; }
    if (n >= 4 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') { *supported = 1; return "PNG"; }
    if (n >= 4 && memcmp(b, "GIF8", 4) == 0) { *supported = 1; return "GIF"; }
    if (n >= 2 && b[0] == 'B' && b[1] == 'M') { *supported = 1; return "BMP"; }
    if (n >= 4 && ((b[0] == 'I' && b[1] == 'I' && b[2] == 0x2A && b[3] == 0) ||
                   (b[0] == 'M' && b[1] == 'M' && b[2] == 0 && b[3] == 0x2A))) {
        *supported = 1;
        return "TIFF";
    }
    if (n >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WEBP", 4) == 0)
        return "WebP";
    if (n >= 12 && memcmp(b + 4, "ftyp", 4) == 0) {
        if (memcmp(b + 8, "avif", 4) == 0 || memcmp(b + 8, "avis", 4) == 0)
            return "AVIF";
        static const char *const heic[] = {"heic", "heix", "hevc", "hevx", "mif1", "msf1"};
        for (int i = 0; i < 6; i++)
            if (memcmp(b + 8, heic[i], 4) == 0)
                return "HEIC";
        return "MP4/видео";
    }
    if (n >= 4 && memcmp(b, "%PDF", 4) == 0)
        return "PDF";
    if (n >= 1 && b[0] == '<')
        return "текст (HTML/SVG), это не картинка";
    return "неизвестный формат";
}

/* Читает первые байты файла и определяет его настоящий формат (название - в name). */
static void sniff_file(const WCHAR *path, char *name, size_t cap, int *supported) {
    BYTE b[16];
    DWORD got = 0;
    memset(b, 0, sizeof(b));
    *supported = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        snprintf(name, cap, "файл не удаётся прочитать (занят другой программой?)");
        return;
    }
    BOOL ok = ReadFile(f, b, sizeof(b), &got, NULL);
    CloseHandle(f);
    if (!ok || got == 0) {
        snprintf(name, cap, "пустой файл");
        return;
    }
    snprintf(name, cap, "%s", sniff_image_format(b, got, supported));
}

/* Подбирает фото борта. Порядок: 1) ваш файл в папке aircraft_photos (интернет не нужен);
 * 2) фото из интернета - только если в acars_settings.ini указан контакт; 3) если ничего нет,
 * панель рисует силуэт по типу самолёта (это делает draw_panel_photo).
 * user_click = 1 - борт только что выбрали щелчком: папку со своими фото перепроверяем (мог
 * появиться новый файл); при вызове по таймеру она проверяется только если ячейки ещё нет. */
static void photo_request_for(const MapAircraft *a, int user_click) {
    char key[48];
    if (!photo_make_key(a, key, sizeof(key)))
        return;
    PhotoSlot *s = photo_find(key);
    DWORD now = GetTickCount();

    if (s && s->state == PH_BAD && !user_click)
        return; /* сообщение о негодном файле не затираем по таймеру - оно исчезнет после нового щелчка */

    if (user_click || !s) {
        WCHAR path[MAX_PATH + 240], lname[200];
        int rule = 0;
        if (find_local_photo(a, path, &rule, lname)) {
            int w = 0, h = 0;
            HBITMAP hb = load_local_bitmap(path, &w, &h);
            if (hb) {
                if (!s)
                    s = photo_alloc(key);
                if (s->bmp)
                    DeleteObject(s->bmp);
                s->bmp = hb;
                s->w = w;
                s->h = h;
                s->local = 1;
                s->rule = rule;
                wcsncpy(s->lname, lname, 199);
                s->lname[199] = L'\0';
                s->state = PH_READY;
                s->stamp = now;
                s->photographer[0] = '\0';
                s->link[0] = '\0';
                s->use = ++g_photo_use;
                return;
            }
            /* файл есть, а картинкой не открылся (битый, WebP/HEIC...) - не молчим, панель скажет */
            if (!s)
                s = photo_alloc(key);
            if (s->bmp) {
                DeleteObject(s->bmp);
                s->bmp = NULL;
            }
            s->local = 0;
            s->state = PH_BAD;
            s->stamp = now;
            sniff_file(path, s->photographer, sizeof(s->photographer), &s->fmt_ok); /* что на самом деле лежит в файле */
            return;
        }
        if (s && s->local) { /* файл убрали - забываем его, дальше как обычно */
            if (s->bmp)
                DeleteObject(s->bmp);
            s->bmp = NULL;
            s->local = 0;
            s->state = PH_OFF;
        }
    }
    if (s && s->local) {
        s->use = ++g_photo_use;
        return; /* ваше фото уже показано - из интернета ничего не нужно */
    }

    char contact[160];
    if (!read_contact(contact, sizeof(contact))) { /* интернет-фото не включено: останется силуэт */
        if (!s)
            s = photo_alloc(key);
        if (s->state != PH_READY && s->state != PH_PENDING) {
            s->state = PH_OFF;
            s->stamp = now;
        }
        return;
    }
    if (s) {
        s->use = ++g_photo_use;
        if (s->state == PH_PENDING && now - s->stamp < PHOTO_PENDING_MS)
            return;
        if ((s->state == PH_READY || s->state == PH_NONE) && now - s->stamp < PHOTO_TTL_MS)
            return;
        if (s->state == PH_ERROR && now - s->stamp < PHOTO_ERR_RETRY_MS)
            return;
        if (s->bmp) {
            DeleteObject(s->bmp);
            s->bmp = NULL;
        }
    } else {
        s = photo_alloc(key);
    }
    s->state = PH_PENDING;
    s->stamp = now;
    EnterCriticalSection(&g_pcs);
    snprintf(g_preq, sizeof(g_preq), "%s", key); /* если там уже был другой запрос - побеждает последний */
    LeaveCriticalSection(&g_pcs);
    SetEvent(g_pevt);
}

/* Результат фонового потока пришёл в окно - раскладываем по ячейке. */
static void photo_deliver(PhotoResult *r) {
    PhotoSlot *s = photo_find(r->key);
    if (s) {
        s->stamp = GetTickCount();
        if (r->status == PH_READY && r->bytes) {
            int w = 0, h = 0;
            HBITMAP hb = decode_image_bytes(r->bytes, r->nbytes, &w, &h);
            if (hb) {
                if (s->bmp)
                    DeleteObject(s->bmp);
                s->bmp = hb;
                s->w = w;
                s->h = h;
                s->local = 0;
                snprintf(s->photographer, sizeof(s->photographer), "%s", r->photographer);
                snprintf(s->link, sizeof(s->link), "%s", r->link);
                s->state = PH_READY;
            } else {
                s->state = PH_ERROR;
            }
        } else {
            s->state = (r->status == PH_NONE) ? PH_NONE : PH_ERROR;
        }
    }
    free(r->bytes); /* картинка в памяти больше не нужна: остался только готовый HBITMAP */
    free(r);
}

static void open_photo_link(void) {
    if (!starts_with(g_photo_link, PHOTO_PAGE_PREFIX)) /* открываем только страницу Planespotters */
        return;
    WCHAR w[320];
    u8_to_w(g_photo_link, w, 320);
    ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

/* Фото включается один раз: пользователь вписывает свой контакт в acars_settings.ini. */
static void setup_photo_contact(HWND hwnd) {
    int r = MessageBoxW(
        hwnd,
        L"Фото самолётов берутся с сайта planespotters.net (бесплатный публичный API).\n\n"
        L"По правилам сервиса программа должна представляться, и в запросе должен быть контакт - "
        L"адрес электронной почты или сайта. Придумывать за вас контакт программа не может, "
        L"поэтому его нужно указать самому.\n\n"
        L"Откроется файл acars_settings.ini: после строки \"Contact=\" впишите свою почту или "
        L"адрес сайта, сохраните файл и закройте Блокнот. Ничего больше не нужно.\n\n"
        L"Наружу уходит только код или регистрация выбранного борта и этот контакт. "
        L"Открыть файл?",
        L"Фото самолётов", MB_YESNO | MB_ICONINFORMATION);
    if (r != IDYES)
        return;
    WCHAR ini[MAX_PATH + 32], cur[8];
    cwd_file(L"acars_settings.ini", ini);
    GetPrivateProfileStringW(L"Photos", L"Contact", L"", cur, 8, ini);
    if (!cur[0])
        WritePrivateProfileStringW(L"Photos", L"Contact", L"", ini); /* создаёт строку "Contact=" */
    ShellExecuteW(hwnd, L"open", L"notepad.exe", ini, NULL, SW_SHOWNORMAL);
}

/* ---------------------------------------------------------------- */
/* Силуэт самолёта по типу (рисуется самой программой, без интернета) */
/* ---------------------------------------------------------------- */
/* Вид сбоку, нос справа, в условной коробке шириной SIL_W и высотой SIL_H (целые числа - так
 * проще проверять границы). Три вида: узкофюзеляжный реактивный (A320, B737, E190, SSJ100...),
 * широкофюзеляжный (A330, B777, B787, A350, Ил-96...) и турбовинтовой (ATR, Dash 8, Ан-24...).
 * Это схема ТИПА, а не фото конкретного борта. Оттенки: 0 - корпус, 1 - тёмный (крыло,
 * двигатель), 2 - окна. */
#define SIL_W 200
#define SIL_H 64
#define SIL_MAXPTS 16
typedef struct {
    const int (*p)[2];
    int n;
    int shade;
} SilPart;
#define SIL_PTS(a) a, (int)(sizeof(a) / sizeof((a)[0]))

static const int N_WING[][2] = {{96, 43}, {128, 43}, {88, 58}, {74, 58}};
static const int N_ENG[][2] = {{98, 46}, {116, 46}, {120, 49}, {116, 54}, {98, 54}, {94, 50}};
static const int N_BODY[][2] = {{8, 23},  {156, 21}, {170, 22}, {182, 25}, {190, 29}, {194, 33}, {192, 37},
                                {186, 41}, {176, 43}, {164, 44}, {64, 44}, {40, 41}, {16, 34}};
static const int N_FIN[][2] = {{8, 23}, {2, 2}, {18, 2}, {52, 21}};
static const int N_TAIL[][2] = {{12, 31}, {4, 35}, {30, 36}, {44, 33}};
static const int N_CABIN[][2] = {{172, 26}, {184, 28}, {188, 31}, {174, 31}};
static const int N_WIN[][2] = {{40, 30}, {150, 30}, {150, 33}, {40, 33}};
static const SilPart SIL_NARROW[] = {{SIL_PTS(N_WING), 1}, {SIL_PTS(N_ENG), 1}, {SIL_PTS(N_BODY), 0},
                                     {SIL_PTS(N_FIN), 0},  {SIL_PTS(N_TAIL), 1}, {SIL_PTS(N_CABIN), 2},
                                     {SIL_PTS(N_WIN), 2}};

static const int W_WING[][2] = {{82, 45}, {126, 45}, {78, 63}, {58, 63}};
static const int W_ENG[][2] = {{84, 48}, {108, 48}, {114, 52}, {108, 59}, {84, 59}, {78, 53}};
static const int W_BODY[][2] = {{6, 19},   {150, 17}, {168, 19}, {182, 24}, {191, 29}, {196, 34}, {193, 39},
                                {185, 43}, {172, 45}, {160, 46}, {60, 46},  {34, 43},  {12, 34}};
static const int W_FIN[][2] = {{6, 19}, {0, 0}, {22, 0}, {60, 17}};
static const int W_TAIL[][2] = {{12, 32}, {2, 38}, {36, 40}, {52, 35}};
static const int W_CABIN[][2] = {{172, 24}, {186, 27}, {190, 31}, {174, 31}};
static const int W_WIN[][2] = {{30, 29}, {148, 29}, {148, 32}, {30, 32}};
static const SilPart SIL_WIDE[] = {{SIL_PTS(W_WING), 1}, {SIL_PTS(W_ENG), 1}, {SIL_PTS(W_BODY), 0},
                                   {SIL_PTS(W_FIN), 0},  {SIL_PTS(W_TAIL), 1}, {SIL_PTS(W_CABIN), 2},
                                   {SIL_PTS(W_WIN), 2}};

static const int T_WING[][2] = {{82, 26}, {118, 26}, {112, 18}, {88, 18}};
static const int T_ENG[][2] = {{74, 20}, {108, 20}, {114, 25}, {108, 32}, {74, 32}, {68, 26}};
static const int T_PROP[][2] = {{64, 10}, {68, 10}, {68, 40}, {64, 40}};
static const int T_BODY[][2] = {{8, 26},   {130, 25}, {150, 26}, {164, 29}, {172, 33}, {174, 37}, {170, 41},
                                {160, 43}, {148, 44}, {56, 44},  {34, 41},  {14, 35}};
static const int T_FIN[][2] = {{8, 26}, {4, 6}, {20, 6}, {46, 25}};
static const int T_TAIL[][2] = {{0, 6}, {34, 6}, {34, 11}, {0, 11}};
static const int T_CABIN[][2] = {{158, 28}, {168, 30}, {170, 33}, {160, 33}};
static const int T_WIN[][2] = {{26, 32}, {124, 32}, {124, 35}, {26, 35}};
static const SilPart SIL_PROP[] = {{SIL_PTS(T_WING), 1}, {SIL_PTS(T_ENG), 1}, {SIL_PTS(T_PROP), 1},
                                   {SIL_PTS(T_BODY), 0}, {SIL_PTS(T_FIN), 0}, {SIL_PTS(T_TAIL), 1},
                                   {SIL_PTS(T_CABIN), 2}, {SIL_PTS(T_WIN), 2}};

/* 0 - узкофюзеляжный реактивный (и всё неизвестное), 1 - широкофюзеляжный, 2 - турбовинтовой.
 * По коду типа ИКАО (A320, B77W, AT72...), регистр не важен. */
static int type_category(const char *t) {
    char u[8];
    size_t n = 0;
    for (; t[n] && n < sizeof(u) - 1; n++)
        u[n] = (char)toupper((unsigned char)t[n]);
    u[n] = '\0';
    static const char *const wide[] = {"A30", "A33", "A34", "A35", "A38", "B74", "B76", "B77",
                                       "B78", "IL9", "DC1", "MD1", "A3ST"};
    static const char *const prop[] = {"AT3", "AT4", "AT5", "AT7", "DH8", "DHC", "SF3", "B19", "AN2",
                                       "L41", "E12", "F50", "F27", "JS3", "JS4", "D32", "PC1", "C20", "SB2"};
    if (strcmp(u, "A310") == 0)
        return 1; /* у A310 префикс A31 общий с узкофюзеляжными A318/A319, поэтому отдельно */
    for (size_t i = 0; i < sizeof(wide) / sizeof(wide[0]); i++)
        if (strncmp(u, wide[i], strlen(wide[i])) == 0)
            return 1;
    for (size_t i = 0; i < sizeof(prop) / sizeof(prop[0]); i++)
        if (strncmp(u, prop[i], strlen(prop[i])) == 0)
            return 2;
    return 0;
}

static const char *const SIL_NAMES[] = {"узкофюзеляжный реактивный", "широкофюзеляжный", "турбовинтовой"};

/* Рисует силуэт категории cat в прямоугольнике шириной w (высота w*SIL_H/SIL_W) с левым верхним
 * углом (x, y). */
static void draw_silhouette(HDC dc, int x, int y, int w, int cat) {
    const SilPart *parts = cat == 1 ? SIL_WIDE : (cat == 2 ? SIL_PROP : SIL_NARROW);
    int n = cat == 1 ? (int)(sizeof(SIL_WIDE) / sizeof(SIL_WIDE[0]))
                     : (cat == 2 ? (int)(sizeof(SIL_PROP) / sizeof(SIL_PROP[0]))
                                 : (int)(sizeof(SIL_NARROW) / sizeof(SIL_NARROW[0])));
    double sc = (double)w / SIL_W;
    static const COLORREF shade[3] = {RGB(150, 168, 184), RGB(92, 108, 126), RGB(34, 46, 60)};
    for (int k = 0; k < n; k++) {
        POINT pts[SIL_MAXPTS];
        int m = parts[k].n < SIL_MAXPTS ? parts[k].n : SIL_MAXPTS;
        for (int i = 0; i < m; i++) {
            pts[i].x = x + (LONG)(parts[k].p[i][0] * sc + 0.5);
            pts[i].y = y + (LONG)(parts[k].p[i][1] * sc + 0.5);
        }
        HBRUSH br = CreateSolidBrush(shade[parts[k].shade]);
        HPEN pen = CreatePen(PS_SOLID, 1, shade[parts[k].shade == 2 ? 2 : 1]);
        HGDIOBJ ob = SelectObject(dc, br);
        HGDIOBJ op = SelectObject(dc, pen);
        Polygon(dc, pts, m);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(br);
        DeleteObject(pen);
    }
}

/* Сколько строк списка бортов показывать: когда выбран борт, нужно место под фото и данные,
 * поэтому список сокращается (остальные борта по-прежнему выбираются кликом на карте). */
static int panel_rows_shown(void) {
    int cap = find_sel() >= 0 ? 4 : 10;
    return g_nac < cap ? g_nac : cap;
}

/* Прокрутка списка бортов: g_list_top - индекс первой видимой строки. Видимых строк мало (4 или 10, остальное
 * место нужно под карточку борта), поэтому остальные борта доступны колёсиком мыши над списком, а выбранный по
 * клику на карте борт автоматически прокручивается в видимую часть. */
static int g_list_top;

static void list_clamp_top(void) {
    int shown = panel_rows_shown();
    int max_top = g_nac - shown;
    if (max_top < 0) max_top = 0;
    if (g_list_top > max_top) g_list_top = max_top;
    if (g_list_top < 0) g_list_top = 0;
}

static void list_ensure_visible(int idx) {
    int shown = panel_rows_shown();
    if (idx < g_list_top) g_list_top = idx;
    else if (idx >= g_list_top + shown) g_list_top = idx - shown + 1;
    list_clamp_top();
}

#define LIST_FIRST_ROW_TOP (MAP_TOOLBAR_H + 8 + 28 - 2) /* то же, что y-2 первой строки в draw_panel */

/* Рисует текст с переносом по словам и возвращает ЗАНЯТУЮ высоту. DrawText с готовым прямоугольником
 * обрезает хвост, если он не влез (так было с длинным сообщением об ошибке) - здесь сначала измеряем
 * (DT_CALCRECT), потом рисуем. */
static int draw_text_block(HDC dc, int x, int y, int right, const char *u8) {
    WCHAR w[600];
    u8_to_w(u8, w, 600);
    RECT r = {x, y, right, y + 1};
    DrawTextW(dc, w, -1, &r, DT_WORDBREAK | DT_CALCRECT);
    int h = (int)(r.bottom - r.top);
    RECT d = {x, y, right, y + h};
    DrawTextW(dc, w, -1, &d, DT_WORDBREAK);
    return h;
}

/* То же для текста без пробелов (полный путь к файлу): DrawText такое слово не переносит, а обрезает,
 * поэтому рвём по символам - сколько влезло в ширину, столько и в строку. Возвращает высоту. */
static int draw_chars_wrapped(HDC dc, int x, int y, int maxw, const WCHAR *text) {
    SIZE sz;
    GetTextExtentPoint32W(dc, L"Ay", 2, &sz);
    int lh = (int)sz.cy, total = 0;
    const WCHAR *p = text;
    while (*p) {
        int len = (int)wcslen(p), fit = 0;
        SIZE s2;
        GetTextExtentExPointW(dc, p, len, maxw, &fit, NULL, &s2);
        if (fit < 1)
            fit = 1;
        TextOutW(dc, x, y + total, p, fit);
        p += fit;
        total += lh;
    }
    return total ? total : lh;
}

/* Картинка выбранного борта в панели. *py - текущая позиция по вертикали, после вызова
 * сдвигается ниже нарисованного. Что показывается:
 *  1) фото (ваше из aircraft_photos или из интернета) - если есть;
 *  2) иначе силуэт по типу самолёта с подписью, почему фото нет и как его добавить.
 * Запоминает область для кликов: фото из интернета - ссылка на страницу снимка; подсказка
 * "включить фото из интернета" - запуск настройки контакта. */
static void draw_panel_photo(HDC dc, const RECT *pr, int x, int *py, const MapAircraft *a) {
    int iw = (pr->right - pr->left) - 28; /* доступная ширина */
    int y = *py;
    char key[48];
    g_photo_click = 0;
    g_folder_click = 0;
    SelectObject(dc, g_fontSmall);

    PhotoSlot *ps = photo_make_key(a, key, sizeof(key)) ? photo_find(key) : NULL;

    if (ps && ps->state == PH_READY && ps->bmp && ps->w > 0 && ps->h > 0) {
        double sc = (double)iw / ps->w;
        if (sc > 1.6)
            sc = 1.6; /* сильнее растягивать маленькую картинку незачем - будет мыло */
        int dw = (int)(ps->w * sc), dh = (int)(ps->h * sc);
        int maxh = (pr->bottom - y) * 40 / 100; /* фото не должно съедать всю панель */
        if (maxh < 110)
            maxh = 110;
        if (dh > maxh) {
            dh = maxh;
            dw = ps->w * dh / ps->h;
        }

        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, ps->bmp);
        SetStretchBltMode(dc, HALFTONE); /* качественное сжатие/растяжение */
        SetBrushOrgEx(dc, 0, 0, NULL);
        StretchBlt(dc, x, y, dw, dh, mem, 0, 0, ps->w, ps->h, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
        y += dh + 3;

        char credit[200];
        if (ps->local) {
            static const char *const what[] = {"", "этого борта", "модели и компании", "типа и компании", "модели", "типа", "компании"};
            int r = (ps->rule >= 1 && ps->rule <= 6) ? ps->rule : 1;
            WCHAR wc[300];
            char note[120];
            snprintf(note, sizeof(note), r == 1 ? "Ваше фото %s" : "Ваше фото %s (не этого борта)", what[r]);
            u8_to_w(note, wc, 300);
            wcsncat(wc, L": ", 299 - wcslen(wc));
            wcsncat(wc, ps->lname, 299 - wcslen(wc));
            SetTextColor(dc, CLR_DIM);
            RECT cr = {x, y, pr->right - 12, y + 1};
            DrawTextW(dc, wc, -1, &cr, DT_WORDBREAK | DT_CALCRECT);
            int chh = (int)(cr.bottom - cr.top);
            cr.bottom = cr.top + chh;
            DrawTextW(dc, wc, -1, &cr, DT_WORDBREAK);
            *py = y + chh + 8;
            return;
        } else {
            /* подпись фотографа и ссылка на страницу снимка - обязательные условия API */
            g_photo_rc.left = x; g_photo_rc.top = y - dh - 3; g_photo_rc.right = x + dw; g_photo_rc.bottom = y - 3;
            snprintf(g_photo_link, sizeof(g_photo_link), "%s", ps->link);
            g_photo_click = 1;
            snprintf(credit, sizeof(credit), "Фото: %.100s  -  planespotters.net", ps->photographer);
        }
        SetTextColor(dc, CLR_DIM);
        RECT cr = {x, y, pr->right - 12, y + 34};
        put_text_rect(dc, &cr, credit, DT_WORDBREAK);
        *py = y + 36;
        return;
    }

    /* фото нет - рисуем схематичный силуэт по типу */
    int cat = type_category(a->type);
    int sw = iw - 16;
    int sh = sw * SIL_H / SIL_W;
    int boxh = sh + 16;
    RECT box = {x, y, x + iw, y + boxh};
    HBRUSH bb = CreateSolidBrush(RGB(26, 32, 40));
    FillRect(dc, &box, bb);
    DeleteObject(bb);
    HBRUSH fb = CreateSolidBrush(CLR_TILE_LINE);
    FrameRect(dc, &box, fb);
    DeleteObject(fb);
    draw_silhouette(dc, x + 8, y + 8, sw, cat);
    y += boxh + 4;

    const char *why;
    char bad_msg[640] = "";
    if (ps && ps->state == PH_BAD) {
        if (ps->fmt_ok)
            snprintf(bad_msg, sizeof(bad_msg),
                     "Файл найден, формат %.60s распознан, но открыть картинку не удалось: файл повреждён или слишком большой (больше ~8000 пикселей).",
                     ps->photographer);
        else
            snprintf(bad_msg, sizeof(bad_msg),
                     "Файл найден, но внутри у него не JPEG и не PNG, а: %.80s. Программа попробовала открыть его средствами Windows и не смогла - скорее всего, нет кодека (Microsoft Store: \"Расширения для изображений WebP\" / \"HEIF\"). Либо откройте файл в Paint и сохраните как JPEG.",
                     ps->photographer);
    }
    if (ps && ps->state == PH_PENDING)
        why = "Ищу фото в интернете...";
    else if (ps && ps->state == PH_NONE)
        why = "Фото этого борта на planespotters.net не найдено.";
    else if (ps && ps->state == PH_ERROR)
        why = "Фото из интернета сейчас недоступно.";
    else if (ps && ps->state == PH_BAD)
        why = bad_msg; /* ниже: зависит от того, что на самом деле лежит в файле */
    else
        why = "Фото этого борта пока нет.";
    char cap[760]; /* русский текст в UTF-8 - по 2 байта на букву */
    if (a->type[0])
        snprintf(cap, sizeof(cap), "Схема типа %.15s (%s), не фото борта. %s", a->type, SIL_NAMES[cat], why);
    else
        snprintf(cap, sizeof(cap), "Схема (тип самолёта неизвестен), не фото борта. %s", why);
    SetTextColor(dc, CLR_DIM);
    y += draw_text_block(dc, x, y, pr->right - 12, cap) + 4;

    /* Куда положить файл и под каким именем: подсказка составляется по данным ЭТОГО борта. Если файл
     * найден, но не открылся - показываем его путь. */
    {
        WCHAR dir[MAX_PATH + 32], found[MAX_PATH + 240], fname[200], line[MAX_PATH * 2 + 120], hint[420];
        int rule = 0;
        cwd_file(L"aircraft_photos", dir);
        SetTextColor(dc, CLR_DIM);
        if (ps && ps->state == PH_BAD && find_local_photo(a, found, &rule, fname)) {
            y += draw_text_block(dc, x, y, pr->right - 12, "Файл:");
            wsprintfW(line, L"%s", found);
            y += draw_chars_wrapped(dc, x, y, pr->right - 12 - x, line);
        } else {
            y += draw_text_block(dc, x, y, pr->right - 12, "Папка для своих фото:");
            y += draw_chars_wrapped(dc, x, y, pr->right - 12 - x, dir);
            photo_hint_names(a, hint, 420);
            if (hint[0]) {
                y += draw_text_block(dc, x, y + 4, pr->right - 12, "Подойдут имена (по убыванию точности; расширение любое: .jpg .png .webp ...):") + 4;
                y += draw_chars_wrapped(dc, x, y, pr->right - 12 - x, hint);
            }
        }
        y += 6;
    }

    SetTextColor(dc, CLR_RING);
    RECT fr = {x, y, pr->right - 12, y + 18};
    put_text_rect(dc, &fr, "Открыть папку aircraft_photos", DT_SINGLELINE);
    g_folder_rc = fr;
    g_folder_click = 1;
    y += 22;

    if (!contact_ok_cached()) { /* фото из интернета не включено - подсказка, она же кнопка */
        SetTextColor(dc, CLR_AMBER);
        RECT hr = {x, y, pr->right - 12, y + 36};
        put_text_rect(dc, &hr, "Фото из интернета: нажмите здесь, чтобы включить", DT_WORDBREAK);
        g_photo_rc = hr;
        g_photo_click = 2;
        y += 38;
    }
    *py = y + 6;
}

/* ETA борта приходит как "1142" или "11:42" (UTC) -> "11:42 UTC (14:42 МСК)"; не разобрать - как есть */
static void eta_msk_text(const char *eta, char *out, size_t cap) {
    int h = -1, m = -1;
    if (eta) {
        if (strlen(eta) >= 4 && isdigit((unsigned char)eta[0]) && isdigit((unsigned char)eta[1]) &&
            isdigit((unsigned char)eta[2]) && isdigit((unsigned char)eta[3]) && !isdigit((unsigned char)eta[4])) {
            h = (eta[0] - '0') * 10 + eta[1] - '0';
            m = (eta[2] - '0') * 10 + eta[3] - '0';
        } else if (strlen(eta) >= 5 && eta[2] == ':') {
            h = (eta[0] - '0') * 10 + eta[1] - '0';
            m = (eta[3] - '0') * 10 + eta[4] - '0';
        }
    }
    if (h < 0 || h > 23 || m < 0 || m > 59) {
        snprintf(out, cap, "%.15s", eta ? eta : "");
        return;
    }
    snprintf(out, cap, "%02d:%02d UTC (%02d:%02d МСК)", h, m, (h + 3) % 24, m);
}

static void draw_panel(HDC dc, const RECT *client) {
    RECT pr = {client->right - MAP_PANEL_W, MAP_TOOLBAR_H, client->right, client->bottom};
    HBRUSH pb = CreateSolidBrush(CLR_PANEL);
    FillRect(dc, &pr, pb);
    DeleteObject(pb);
    g_photo_click = 0;
    g_folder_click = 0;

    int x = pr.left + 12, y = pr.top + 8;
    char buf[320];
    SetBkMode(dc, TRANSPARENT);

    SelectObject(dc, g_fontBold);
    SetTextColor(dc, CLR_AMBER);
    snprintf(buf, sizeof(buf), "Борта с координатами: %d", g_nac);
    put_text(dc, x, y, buf);
    y += 28;

    SelectObject(dc, g_font);
    if (g_nac == 0) {
        RECT r = {x, y, pr.right - 10, pr.bottom - 10};
        SetTextColor(dc, CLR_DIM);
        put_text_rect(dc, &r,
                      "Пока нет ни одного борта с известными координатами.\n\n"
                      "Координаты приходят в телеметрии ASU/AFL (метки 26/27, поля LATN/LONE). "
                      "Если сообщений такого вида в приёме не было - карта останется пустой.\n\n"
                      "Ранее сохранённый журнал можно загрузить кнопкой \"Импорт JSONL...\" "
                      "в главном окне - борта появятся здесь сами.",
                      DT_WORDBREAK);
        return;
    }

    int sel = find_sel();
    int shown = panel_rows_shown();
    list_clamp_top();
    for (int i = 0; i < shown; i++) {
        int idx = g_list_top + i;
        RECT row = {pr.left + 4, y - 2, pr.right - 10, y + ROW_H - 4};
        if (idx == sel) {
            HBRUSH sb = CreateSolidBrush(RGB(40, 52, 66));
            FillRect(dc, &row, sb);
            DeleteObject(sb);
        }
        {
            RECT chip = {x, y + 3, x + 11, y + 14};
            HBRUSH cb = CreateSolidBrush(plane_color(idx));
            FillRect(dc, &chip, cb);
            DeleteObject(cb);
        }
        SetTextColor(dc, idx == sel ? CLR_AMBER_SEL : CLR_TEXT);
        SelectObject(dc, g_font);
        if (g_ac[idx].alt[0])
            { char at[48]; alt_ft_text(g_ac[idx].alt, at, sizeof(at), NULL);
            snprintf(buf, sizeof(buf), "%.23s   %.47s", g_ac[idx].reg, at); }
        else
            snprintf(buf, sizeof(buf), "%.23s", g_ac[idx].reg);
        put_text(dc, x + 18, y - 1, buf);
        {
            char eb[64] = "", l2[160];
            if (g_ac[idx].eta[0])
                eta_msk_text(g_ac[idx].eta, eb, sizeof eb);
            if (eb[0])
                snprintf(l2, sizeof l2, "следов: %d   ETA %s", g_ac[idx].n_trail, eb);
            else
                snprintf(l2, sizeof l2, "следов: %d", g_ac[idx].n_trail);
            SelectObject(dc, g_fontSmall);
            SetTextColor(dc, idx == sel ? CLR_AMBER_SEL : CLR_DIM);
            put_text(dc, x + 18, y + 17, l2);
            SelectObject(dc, g_font);
        }
        y += ROW_H;
    }
    if (g_nac > shown) {
        /* полоса прокрутки справа от списка */
        int top = LIST_FIRST_ROW_TOP, h = shown * ROW_H;
        RECT track = {pr.right - 8, top, pr.right - 4, top + h};
        HBRUSH tb = CreateSolidBrush(RGB(34, 42, 52));
        FillRect(dc, &track, tb);
        DeleteObject(tb);
        int th = h * shown / g_nac;
        if (th < 14) th = 14;
        int ty = top + (h - th) * g_list_top / (g_nac - shown);
        RECT thumb = {pr.right - 8, ty, pr.right - 4, ty + th};
        HBRUSH hb = CreateSolidBrush(CLR_AMBER);
        FillRect(dc, &thumb, hb);
        DeleteObject(hb);

        SetTextColor(dc, CLR_DIM);
        snprintf(buf, sizeof(buf), "Борта %d-%d из %d (колёсико мыши - прокрутка)",
                 g_list_top + 1, g_list_top + shown, g_nac);
        put_text(dc, x, y, buf);
        y += ROW_H;
    }

    y += 8;
    if (sel < 0) {
        SetTextColor(dc, CLR_DIM);
        put_text(dc, x, y, "Нажмите на самолёт или на строку списка.");
        return;
    }

    const MapAircraft *a = &g_ac[sel];
    MoveToEx(dc, pr.left + 8, y - 4, NULL);
    {
        HPEN lp = CreatePen(PS_SOLID, 1, CLR_TILE_LINE);
        HGDIOBJ op = SelectObject(dc, lp);
        LineTo(dc, pr.right - 8, y - 4);
        SelectObject(dc, op);
        DeleteObject(lp);
    }
    SelectObject(dc, g_fontBold);
    SetTextColor(dc, plane_color(sel));
    put_text(dc, x, y, a->reg);
    y += 26;

    /* маршрут: откуда -> куда (из служебных отчётов Аэрофлота/России; у остальных не передаётся) */
    g_rep_click = 0;
    SelectObject(dc, g_font);
    if (a->flight[0] || a->dep[0] || a->dest[0]) {
        SetTextColor(dc, CLR_TEXT);
        if (a->flight[0]) {
            snprintf(buf, sizeof(buf), "Рейс: %.7s", a->flight);
            put_text(dc, x, y, buf);
            y += 20;
        }
        char al[200], line[320];
        if (a->dep[0]) {
            airport_line(a->dep, al, sizeof al);
            snprintf(line, sizeof line, "Откуда: %s", al);
            RECT r = {x, y, pr.right - 10, y + 40};
            put_text_rect(dc, &r, line, DT_WORDBREAK | DT_END_ELLIPSIS);
            y += 40;
        }
        if (a->dest[0]) {
            airport_line(a->dest, al, sizeof al);
            snprintf(line, sizeof line, "Куда: %s", al);
            RECT r = {x, y, pr.right - 10, y + 40};
            put_text_rect(dc, &r, line, DT_WORDBREAK | DT_END_ELLIPSIS);
            y += 40;
            AirportInfo ap;
            if (route_airport(a->dest, &ap)) {
                snprintf(line, sizeof line, "До %.4s по прямой: %.0f км", a->dest,
                         geo_distance_km(a->lat, a->lon, ap.lat, ap.lon));
                SetTextColor(dc, CLR_DIM);
                put_text(dc, x, y, line);
                y += 20;
            }
        }
    } else {
        SetTextColor(dc, CLR_DIM);
        put_text(dc, x, y, "Маршрут в сообщениях борта не передан");
        y += 20;
    }
    if (g_report_cb) {
        SetTextColor(dc, CLR_AMBER);
        SelectObject(dc, g_fontBold);
        put_text(dc, x, y, xp_is_xp() ? "\u25BA Все сообщения борта... (B)" : "\u25B6 Все сообщения борта... (B)");
        g_rep_rc.left = x; g_rep_rc.top = y; g_rep_rc.right = pr.right - 10; g_rep_rc.bottom = y + 20;
        g_rep_click = 1;
        y += 26;
        SelectObject(dc, g_font);
    }

    draw_panel_photo(dc, &pr, x, &y, a);

    SelectObject(dc, g_font);
    SetTextColor(dc, CLR_TEXT);
    if (a->info[0]) {
        RECT r = {x, y, pr.right - 10, y + 40};
        put_text_rect(dc, &r, a->info, DT_WORDBREAK | DT_END_ELLIPSIS);
        y += 40;
    }
    snprintf(buf, sizeof(buf), "Время: %.31s", a->time);
    put_text(dc, x, y, buf);
    y += 20;
    snprintf(buf, sizeof(buf), "Позиция: %.5f %s, %.5f %s", fabs(a->lat), a->lat >= 0 ? "N" : "S",
             fabs(a->lon), a->lon >= 0 ? "E" : "W");
    {
        RECT r = {x, y, pr.right - 10, y + 40};
        put_text_rect(dc, &r, buf, DT_WORDBREAK);
    }
    y += 22;
    if (a->alt[0]) {
        { char at[48]; alt_ft_text(a->alt, at, sizeof(at), NULL);
        snprintf(buf, sizeof(buf), "Высота: %.47s", at); }
        put_text(dc, x, y, buf);
        y += 20;
    }
    if (a->fuel[0]) {
        snprintf(buf, sizeof(buf), "Топливо: %.15s", a->fuel);
        put_text(dc, x, y, buf);
        y += 20;
    }
    if (a->eta[0]) {
        { char eb[64]; eta_msk_text(a->eta, eb, sizeof eb); snprintf(buf, sizeof(buf), "ETA: %.60s", eb); }
        put_text(dc, x, y, buf);
        y += 20;
    }
    if (a->has_heading)
        snprintf(buf, sizeof(buf), "Курс: %.0f° (по следу)", a->heading);
    else
        snprintf(buf, sizeof(buf), "Курс: неизвестен (нужны 2 разные позиции)");
    put_text(dc, x, y, buf);
    y += 20;
    snprintf(buf, sizeof(buf), "Позиций в следе: %d", a->n_trail);
    put_text(dc, x, y, buf);
    y += 26;

    /* След борта: все известные позиции по порядку приёма. Строка кликабельна (центрирует карту на точке),
     * стрелки вверх/вниз на клавиатуре листают точки. Не помещается целиком - показывается окно вокруг выбранной. */
    g_trk_rows = 0;
    if (a->n_trail > 0) {
        int avail = (pr.bottom - 10 - (y + 22)) / TRK_ROW_H;
        if (avail >= 3) {
            int cur = cur_point(a);
            int rows = a->n_trail < avail ? a->n_trail : avail;
            int first = cur - rows / 2;
            if (first > a->n_trail - rows) first = a->n_trail - rows;
            if (first < 0) first = 0;
            SelectObject(dc, g_fontBold);
            SetTextColor(dc, plane_color(sel));
            if (rows < a->n_trail)
                snprintf(buf, sizeof(buf), "След: точки %d-%d из %d (стрелки ↑/↓)", first + 1, first + rows, a->n_trail);
            else
                snprintf(buf, sizeof(buf), "След: %d точек (клик или стрелки ↑/↓)", a->n_trail);
            put_text(dc, x, y, buf);
            y += 22;
            g_trk_first = first;
            g_trk_rows = rows;
            g_trk_top = y;
            SelectObject(dc, g_fontSmall);
            for (int r = 0; r < rows; r++) {
                int k = first + r;
                RECT rr = {pr.left + 4, y, pr.right - 4, y + TRK_ROW_H};
                if (k == cur) {
                    HBRUSH sb = CreateSolidBrush(RGB(40, 52, 66));
                    FillRect(dc, &rr, sb);
                    DeleteObject(sb);
                }
                char tm[24], at[48] = "";
                time_short(a->trail_time[k], tm, sizeof tm);
                if (a->trail_alt[k][0]) alt_ft_text(a->trail_alt[k], at, sizeof(at), NULL);
                snprintf(buf, sizeof(buf), "%2d  %s  %.3f%c %.3f%c  %.20s", k + 1, tm, fabs(a->trail_lat[k]),
                         a->trail_lat[k] >= 0 ? 'N' : 'S', fabs(a->trail_lon[k]), a->trail_lon[k] >= 0 ? 'E' : 'W', at);
                SetTextColor(dc, k == cur ? CLR_AMBER_SEL : CLR_TEXT);
                put_text(dc, x, y + 1, buf);
                y += TRK_ROW_H;
            }
            y += 6;
        }
    }

    if (y < pr.bottom - 50) { /* пояснение показываем, только если остаётся место */
        SelectObject(dc, g_fontSmall);
        SetTextColor(dc, CLR_DIM);
        RECT nr = {x, y, pr.right - 10, pr.bottom - 8};
        put_text_rect(dc, &nr,
                      "Курс считается по двум последним разным позициям борта - это направление "
                      "перемещения между сообщениями, а не курс самолёта из бортовых систем. "
                      "Высота передана бортом в футах (по характерным значениям, например 36000) - здесь пересчитана в метры.",
                      DT_WORDBREAK);
    }
}

/* Разделитель между картой и панелью: линия и три "бугорка" посередине - видно, что его можно тянуть.
 * Вынесен отдельно: его левая половина лежит на краю карты, и после перерисовки одной карты (при сдвиге) её
 * нужно дорисовать. */
static void draw_splitter(HDC dc, const RECT *rc) {
    int sx = rc->right - MAP_PANEL_W;
    HBRUSH lb = CreateSolidBrush(g_split_drag ? CLR_AMBER : RGB(70, 82, 96));
    RECT line = {sx - 1, MAP_TOOLBAR_H, sx + 1, rc->bottom};
    FillRect(dc, &line, lb);
    int my = (MAP_TOOLBAR_H + rc->bottom) / 2;
    for (int k = -1; k <= 1; k++) {
        RECT dot = {sx - 3, my + k * 9 - 2, sx + 3, my + k * 9 + 2};
        FillRect(dc, &dot, lb);
    }
    DeleteObject(lb);
}

/* ---- полоса проигрывания истории (клавиша T) ---- */
#define RP_BAR_H 54

static void replay_bar_rect(const RECT *client, RECT *r) {
    r->left = 12;
    r->right = client->right - MAP_PANEL_W - 12;
    if (r->right < r->left + 100) r->right = r->left + 100;
    r->bottom = client->bottom - 24;
    r->top = r->bottom - RP_BAR_H;
}

static long replay_t_from_x(const RECT *r, int x) {
    int x0 = r->left + 16, x1 = r->right - 16;
    if (x1 <= x0 || g_rp_max <= g_rp_min) return g_rp_max;
    if (x < x0) x = x0;
    if (x > x1) x = x1;
    return g_rp_min + (long)((double)(x - x0) / (x1 - x0) * (g_rp_max - g_rp_min));
}

static void replay_set_time(HWND hwnd, long t) {
    if (t < g_rp_min) t = g_rp_min;
    if (t > g_rp_max) t = g_rp_max;
    g_rp_t = t;
    refresh_data();
    InvalidateRect(hwnd, NULL, FALSE);
}

static void replay_toggle(HWND hwnd) {
    if (g_rp_on) {
        g_rp_on = 0;
        g_rp_play = 0;
        g_rp_drag = 0;
        KillTimer(hwnd, TIMER_REPLAY);
        refresh_data();
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }
    long a = 0, b = 0;
    if (g_range_cb)
        g_range_cb(&a, &b);
    if (a <= 0 || b <= a) {
        MessageBoxW(hwnd, L"В журнале пока нет сообщений с координатами - проигрывать нечего.", L"История",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    g_rp_min = a;
    g_rp_max = b;
    g_rp_t = b;
    g_rp_on = 1;
    refresh_data();
    InvalidateRect(hwnd, NULL, FALSE);
}

static void draw_replay_bar(HDC dc, const RECT *client) {
    RECT r;
    replay_bar_rect(client, &r);
    HBRUSH bb = CreateSolidBrush(CLR_PANEL);
    FillRect(dc, &r, bb);
    DeleteObject(bb);
    HPEN fp = CreatePen(PS_SOLID, 1, CLR_TILE_LINE);
    HGDIOBJ op = SelectObject(dc, fp), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(fp);

    int x0 = r.left + 16, x1 = r.right - 16, ty = r.bottom - 14;
    HPEN tp = CreatePen(PS_SOLID, 3, RGB(70, 82, 96));
    op = SelectObject(dc, tp);
    MoveToEx(dc, x0, ty, NULL);
    LineTo(dc, x1, ty);
    SelectObject(dc, op);
    DeleteObject(tp);
    int tx = (g_rp_max > g_rp_min) ? x0 + (int)((double)(g_rp_t - g_rp_min) / (g_rp_max - g_rp_min) * (x1 - x0)) : x1;
    HPEN dp = CreatePen(PS_SOLID, 3, CLR_AMBER);
    op = SelectObject(dc, dp);
    MoveToEx(dc, x0, ty, NULL);
    LineTo(dc, tx, ty);
    SelectObject(dc, op);
    DeleteObject(dp);
    HBRUSH hb = CreateSolidBrush(CLR_AMBER);
    ob = SelectObject(dc, hb);
    HPEN np = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    op = SelectObject(dc, np);
    Rectangle(dc, tx - 5, ty - 9, tx + 6, ty + 10);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(hb);
    DeleteObject(np);

    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_fontBold);
    SetTextColor(dc, CLR_AMBER);
    char cur[24], a[24], b[24], line[200];
    geo_time_format(g_rp_t, cur);
    geo_time_format(g_rp_min, a);
    geo_time_format(g_rp_max, b);
    snprintf(line, sizeof line, "Состояние на %.19s (местное)%s   бортов: %d", cur, g_rp_play ? "   (проигрывание)" : "", g_nac);
    put_text(dc, r.left + 10, r.top + 4, line);
    SelectObject(dc, g_fontSmall);
    SetTextColor(dc, CLR_DIM);
    RECT hr = {r.left + 10, r.top + 24, r.right - 8, r.top + 40};
    snprintf(line, sizeof line, "%.16s  ...  %.16s      \xE2\x86\x90/\xE2\x86\x92 \xC2\xB1" "10 мин (Ctrl - 1 ч)   Пробел - проигрывание   T - вернуться к текущему",
             a, b);
    put_text_rect(dc, &hr, line, DT_SINGLELINE | DT_NOPREFIX);
}

static void paint_all(HDC dc, const RECT *rc) {
    HBRUSH bg = CreateSolidBrush(CLR_BG);
    FillRect(dc, rc, bg);
    DeleteObject(bg);

    /* панель инструментов: кнопки - дочерние окна, здесь только подсказка */
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_fontSmall);
    SetTextColor(dc, CLR_DIM);

    RECT m;
    m.left = 0;
    m.top = MAP_TOOLBAR_H;
    m.right = rc->right - MAP_PANEL_W;
    m.bottom = rc->bottom;
    if (m.right < m.left + 120) m.right = m.left + 120;
    if (m.bottom < m.top + 120) m.bottom = m.top + 120;
    draw_map(dc, &m);
    if (g_rp_on)
        draw_replay_bar(dc, rc);
    draw_panel(dc, rc);

    draw_splitter(dc, rc);
}

/* ---------------------------------------------------------------- */
/* Взаимодействие                                                   */
/* ---------------------------------------------------------------- */
static void select_by_index(int idx, int center) {
    if (idx < 0 || idx >= g_nac) {
        g_sel_reg[0] = '\0';
        return;
    }
    strncpy(g_sel_reg, g_ac[idx].reg, sizeof(g_sel_reg) - 1);
    g_sel_reg[sizeof(g_sel_reg) - 1] = '\0';
    g_sel_pt = -1;
    list_ensure_visible(idx);
    photo_request_for(&g_ac[idx], 1); /* фото подбирается только для выбранного борта, по щелчку */
    if (center) {
        g_clat = clamp_lat_view(g_ac[idx].lat);
        g_clon = wrap_lon(g_ac[idx].lon);
    }
}

static int hit_test_plane(HWND hwnd, int mx, int my) {
    RECT m;
    map_rect(hwnd, &m);
    int best = -1;
    double bestd = 18.0 * 18.0;
    for (int i = 0; i < g_nac; i++) {
        int sx, sy;
        ll_to_screen(&m, g_ac[i].lat, g_ac[i].lon, &sx, &sy);
        double d = (double)(sx - mx) * (sx - mx) + (double)(sy - my) * (sy - my);
        if (d <= bestd) {
            bestd = d;
            best = i;
        }
    }
    return best;
}

/* Точка следа выбранного борта под курсором (кроме последней - там сам самолёт), -1 - нет. */
static int hit_test_trail(HWND hwnd, int mx, int my) {
    int sel = find_sel();
    if (sel < 0 || !g_show_trails)
        return -1;
    RECT m;
    map_rect(hwnd, &m);
    int best = -1;
    double bestd = 12.0 * 12.0;
    for (int k = 0; k < g_ac[sel].n_trail - 1; k++) {
        int sx, sy;
        ll_to_screen(&m, g_ac[sel].trail_lat[k], g_ac[sel].trail_lon[k], &sx, &sy);
        double d = (double)(sx - mx) * (sx - mx) + (double)(sy - my) * (sy - my);
        if (d <= bestd) {
            bestd = d;
            best = k;
        }
    }
    return best;
}

static void center_on_point(const MapAircraft *a, int k) {
    if (k < 0 || k >= a->n_trail)
        return;
    g_clat = clamp_lat_view(a->trail_lat[k]);
    g_clon = wrap_lon(a->trail_lon[k]);
}

static void invalidate_view(HWND hwnd);

/* Показать на карте все найденные борта: центр и масштаб по их границам. */
static void focus_matches(HWND hwnd) {
    RECT m;
    map_rect(hwnd, &m);
    int have = 0;
    double mnla = 0, mxla = 0, mnlo = 0, mxlo = 0;
    for (int i = 0; i < g_nac; i++) {
        if (!ac_matches_search(&g_ac[i])) continue;
        double la = g_ac[i].lat, lo = g_ac[i].lon;
        if (!have) { mnla = mxla = la; mnlo = mxlo = lo; have = 1; }
        else {
            if (la < mnla) mnla = la;
            if (la > mxla) mxla = la;
            if (lo < mnlo) mnlo = lo;
            if (lo > mxlo) mxlo = lo;
        }
    }
    if (!have) return;
    int proj = kStyles[g_style].proj;
    int aw = m.right - m.left - 160, ah = m.bottom - m.top - 160;
    if (aw < 50) aw = 50;
    if (ah < 50) ah = 50;
    int z = 8; /* один борт - достаточно крупно, чтобы окрестности были видны */
    if (mxla - mnla > 1e-3 || mxlo - mnlo > 1e-3) {
        for (z = 14; z > 2; z--) {
            double dx = geo_world_xp(mxlo, z, proj) - geo_world_xp(mnlo, z, proj);
            double dy = geo_world_yp(mnla, z, proj) - geo_world_yp(mxla, z, proj);
            if (dx <= aw && dy <= ah) break;
        }
        if (z > 9) z = 9;
    }
    if (z < zoom_lo()) z = zoom_lo();
    if (z > zoom_hi()) z = zoom_hi();
    g_zoom = z;
    g_clat = clamp_lat_view((mnla + mxla) / 2.0);
    g_clon = wrap_lon((mnlo + mxlo) / 2.0);
}

/* Читает запрос из поля. advance=0 - набор текста: один найденный борт выбирается и камера переезжает к нему,
 * несколько - камера охватывает их все; advance=1 (Enter) - выбрать следующий найденный борт по кругу. */
static void search_apply(HWND hwnd, int advance) {
    WCHAR w[80];
    char u[200];
    w[0] = 0;
    if (g_edSearch)
        GetWindowTextW(g_edSearch, w, 80);
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u, NULL, NULL))
        u[0] = '\0';
    char *b = u;
    while (*b == ' ') b++;
    size_t n = strlen(b);
    while (n && b[n - 1] == ' ') b[--n] = '\0';
    char low[64];
    lower_ascii(low, sizeof low, b);
    strcpy(g_search, low);
    norm_reg(g_search_n, sizeof g_search_n, low);
    g_search_cnt = 0;
    int first = -1, nextsel = -1, sel = find_sel();
    for (int i = 0; i < g_nac; i++) {
        if (!ac_matches_search(&g_ac[i])) continue;
        g_search_cnt++;
        if (first < 0) first = i;
        if (nextsel < 0 && sel >= 0 && i > sel) nextsel = i;
    }
    if (g_search_cnt > 0) {
        if (advance) {
            int pick = nextsel >= 0 ? nextsel : first;
            select_by_index(pick, 1);
            if (g_search_cnt > 1 && g_zoom < 8) g_zoom = 8 < zoom_hi() ? 8 : zoom_hi();
            if (g_zoom < zoom_lo()) g_zoom = zoom_lo();
        } else if (g_search_cnt == 1) {
            select_by_index(first, 0);
            focus_matches(hwnd);
        } else
            focus_matches(hwnd);
    }
    invalidate_view(hwnd);
}

static LRESULT CALLBACK SearchEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CHAR && (wp == '\r' || wp == 27)) {
        HWND map = GetParent(h);
        if (wp == '\r')
            search_apply(map, 1);
        else
            SetWindowTextW(h, L"");
        SetFocus(map);
        return 0;
    }
    return CallWindowProcW(g_origSearchProc, h, msg, wp, lp);
}

static void open_in_browser(void) {
    int sel = find_sel();
    if (sel < 0 && g_nac > 0)
        sel = 0;
    if (sel < 0)
        return;
    char url[256], latb[32], lonb[32];
    snprintf(latb, sizeof(latb), "%.6f", g_ac[sel].lat);
    snprintf(lonb, sizeof(lonb), "%.6f", g_ac[sel].lon);
    snprintf(url, sizeof(url), "https://www.openstreetmap.org/?mlat=%.31s&mlon=%.31s#map=12/%.31s/%.31s",
             latb, lonb, latb, lonb);
    WCHAR wurl[256];
    u8_to_w(url, wurl, 256);
    ShellExecuteW(NULL, L"open", wurl, NULL, NULL, SW_SHOWNORMAL);
}

static int in_split_zone(HWND hwnd, int mx, int my) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int sx = rc.right - MAP_PANEL_W;
    return my >= MAP_TOOLBAR_H && mx >= sx - SPLIT_GRAB && mx <= sx + SPLIT_GRAB;
}

static void save_panel_w(void) {
    WCHAR ini[MAX_PATH + 32], v[16];
    cwd_file(L"acars_settings.ini", ini);
    wsprintfW(v, L"%d", g_panel_w);
    WritePrivateProfileStringW(L"Map", L"PanelWidth", v, ini);
}

/* Постоянный буфер кадра: рисуем в него, потом одним BitBlt на экран. Раньше битмап размером с окно
 * создавался и удалялся при КАЖДОЙ перерисовке (десятки раз в секунду при сдвиге). */
static HDC g_bbdc;
static HBITMAP g_bbbmp;
static HGDIOBJ g_bbold;
static int g_bbw, g_bbh;

/* Готовит буфер нужного размера. 1 - буфер новый (рисовать надо всё), 0 - прежний, -1 - не вышло. */
static int bb_ensure(HDC ref, int w, int h) {
    if (g_bbdc && g_bbw == w && g_bbh == h)
        return 0;
    if (g_bbdc) {
        SelectObject(g_bbdc, g_bbold);
        DeleteObject(g_bbbmp);
        DeleteDC(g_bbdc);
        g_bbdc = NULL;
        g_bbbmp = NULL;
    }
    g_bbdc = CreateCompatibleDC(ref);
    g_bbbmp = g_bbdc ? CreateCompatibleBitmap(ref, w, h) : NULL;
    if (!g_bbdc || !g_bbbmp) {
        if (g_bbbmp)
            DeleteObject(g_bbbmp);
        if (g_bbdc)
            DeleteDC(g_bbdc);
        g_bbdc = NULL;
        g_bbbmp = NULL;
        return -1;
    }
    g_bbold = SelectObject(g_bbdc, g_bbbmp);
    g_bbw = w;
    g_bbh = h;
    return 1;
}

/* Перерисовать только карту: при сдвиге, масштабе и приходе тайла панель не меняется, а перерисовка её
 * (текст, сжатое фото) на каждый кадр сдвига только тормозила бы. */
static void invalidate_view(HWND hwnd) {
    RECT m;
    map_rect(hwnd, &m);
    InvalidateRect(hwnd, &m, FALSE);
}

static void invalidate_map(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    rc.top = MAP_TOOLBAR_H;
    InvalidateRect(hwnd, &rc, FALSE);
}

static LRESULT CALLBACK MapWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1; /* всё рисуем сами в paint_all - иначе будет мерцание */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (rc.right > 0 && rc.bottom > 0) {
            int fresh = bb_ensure(hdc, rc.right, rc.bottom);
            if (fresh < 0) { /* буфер не создался - рисуем сразу на экран (будет мерцать, но работать) */
                paint_all(hdc, &rc);
            } else {
                RECT m;
                map_rect(hwnd, &m);
                int map_only = !fresh && ps.rcPaint.left >= m.left && ps.rcPaint.top >= m.top &&
                               ps.rcPaint.right <= m.right && ps.rcPaint.bottom <= m.bottom;
                if (map_only) { /* обновилась только карта - панель в буфере уже нарисована */
                    draw_map(g_bbdc, &m);
                    draw_splitter(g_bbdc, &rc);
                } else {
                    paint_all(g_bbdc, &rc);
                }
                int pw = ps.rcPaint.right - ps.rcPaint.left, ph2 = ps.rcPaint.bottom - ps.rcPaint.top;
                if (pw > 0 && ph2 > 0)
                    BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top, pw, ph2, g_bbdc, ps.rcPaint.left, ps.rcPaint.top,
                           SRCCOPY);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
        g_panel_w = clamp_panel_w(g_panel_w, (int)LOWORD(lParam));
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (g_split_drag || in_split_zone(hwnd, pt.x, pt.y)) {
                SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_SIZEWE)); /* стрелка влево-вправо над разделителем */
                return TRUE;
            }
            if ((g_photo_click && pt.x >= g_photo_rc.left && pt.x < g_photo_rc.right &&
                 pt.y >= g_photo_rc.top && pt.y < g_photo_rc.bottom) ||
                (g_folder_click && pt.x >= g_folder_rc.left && pt.x < g_folder_rc.right &&
                 pt.y >= g_folder_rc.top && pt.y < g_folder_rc.bottom)) {
                SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND)); /* фото - ссылка */
                return TRUE;
            }
        }
        break;

    case WM_APP_PHOTO:
        photo_deliver((PhotoResult *)lParam);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, CLR_TEXT);
        SetBkColor(hdc, CLR_BG);
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)g_brToolbar;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_MAP_SEARCH:
            if (HIWORD(wParam) == EN_CHANGE)
                search_apply(hwnd, 0);
            return 0; /* фокус не отнимаем: иначе после первой же буквы поле перестаёт принимать ввод */
        case IDC_MAP_FIT:
            fit_view(hwnd);
            invalidate_view(hwnd);
            break;
        case IDC_MAP_ZIN:
        case IDC_MAP_ZOUT: {
            RECT m;
            map_rect(hwnd, &m);
            set_zoom_at(&m, g_zoom + (LOWORD(wParam) == IDC_MAP_ZIN ? 1 : -1),
                        (m.left + m.right) / 2, (m.top + m.bottom) / 2);
            invalidate_view(hwnd);
            break;
        }
        case IDC_MAP_TRAIL:
            g_show_trails = (SendMessageW(g_chkTrail, BM_GETCHECK, 0, 0) == BST_CHECKED);
            invalidate_view(hwnd);
            break;
        case IDC_MAP_WEB:
            open_in_browser();
            break;
        case IDC_MAP_SAS:
            if (kStyles[g_style].local && pick_sas_folder(hwnd, g_style)) {
                clamp_zoom();
                clear_tile_state();
                invalidate_map(hwnd);
            }
            break;
        case IDC_MAP_OVR:
            if (kStyles[g_style].has_overlay && pick_overlay_folder(hwnd, g_style)) {
                clear_tile_state();
                invalidate_map(hwnd);
            }
            break;
        case IDC_MAP_STYLE:
            if (HIWORD(wParam) == CBN_SELCHANGE)
                switch_style(hwnd, (int)SendMessageW(g_cmbStyle, CB_GETCURSEL, 0, 0));
            return 0; /* фокус не отнимаем: иначе выпадающий список закроется сам */
        }
        SetFocus(hwnd);
        return 0;

    case WM_LBUTTONDOWN: {
        int mx = MV_GET_X(lParam), my = MV_GET_Y(lParam);
        SetFocus(hwnd);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (in_split_zone(hwnd, mx, my)) { /* начали тянуть разделитель */
            g_split_drag = 1;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (g_rp_on) {
            RECT br;
            replay_bar_rect(&rc, &br);
            if (mx >= br.left && mx < br.right && my >= br.top && my < br.bottom) { /* ползунок времени */
                g_rp_drag = 1;
                g_rp_play = 0;
                KillTimer(hwnd, TIMER_REPLAY);
                SetCapture(hwnd);
                replay_set_time(hwnd, replay_t_from_x(&br, mx));
                return 0;
            }
        }
        if (my >= MAP_TOOLBAR_H && mx >= rc.right - MAP_PANEL_W) {
            if (g_folder_click && mx >= g_folder_rc.left && mx < g_folder_rc.right &&
                my >= g_folder_rc.top && my < g_folder_rc.bottom) {
                open_photo_dir();
                return 0;
            }
            if (g_rep_click && g_report_cb && mx >= g_rep_rc.left && mx < g_rep_rc.right &&
                my >= g_rep_rc.top && my < g_rep_rc.bottom) {
                int si = find_sel();
                if (si >= 0)
                    g_report_cb(hwnd, g_ac[si].reg);
                return 0;
            }
            if (g_photo_click && mx >= g_photo_rc.left && mx < g_photo_rc.right &&
                my >= g_photo_rc.top && my < g_photo_rc.bottom) {
                if (g_photo_click == 1)
                    open_photo_link(); /* фото - ссылка на страницу снимка */
                else
                    setup_photo_contact(hwnd);
                return 0;
            }
            /* клик по строке таблицы следа - выбрать точку и показать её на карте */
            if (g_trk_rows > 0 && my >= g_trk_top && my < g_trk_top + g_trk_rows * TRK_ROW_H) {
                int sel_i = find_sel();
                if (sel_i >= 0) {
                    g_sel_pt = g_trk_first + (my - g_trk_top) / TRK_ROW_H;
                    center_on_point(&g_ac[sel_i], g_sel_pt);
                    invalidate_map(hwnd);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            }
            /* клик по строке списка бортов */
            int first_row_top = LIST_FIRST_ROW_TOP;
            int row = my >= first_row_top ? (my - first_row_top) / ROW_H : -1;
            if (g_nac > panel_rows_shown() && mx >= rc.right - 14 && row >= 0 &&
                row < panel_rows_shown()) {
                /* клик по полосе прокрутки - перейти в соответствующее место списка */
                int span = panel_rows_shown() * ROW_H;
                g_list_top = (my - first_row_top) * (g_nac - panel_rows_shown()) / span;
                list_clamp_top();
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (row >= 0 && g_list_top + row < g_nac && row < panel_rows_shown()) {
                select_by_index(g_list_top + row, 1);
                invalidate_map(hwnd);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        if (my >= MAP_TOOLBAR_H) {
            int tp = hit_test_trail(hwnd, mx, my);
            if (tp >= 0) {
                g_sel_pt = tp;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            int hit = hit_test_plane(hwnd, mx, my);
            if (hit >= 0 && hit == find_sel() && g_ac[hit].n_trail > 0) {
                g_sel_pt = g_ac[hit].n_trail - 1; /* повторный клик по выбранному самолёту - его последняя точка */
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (hit >= 0) {
                select_by_index(hit, 0);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            RECT m;
            map_rect(hwnd, &m);
            g_dragging = 1;
            g_drag_x = mx;
            g_drag_y = my;
            g_drag_wx = MV_WX(g_clon, g_zoom);
            g_drag_wy = MV_WY(g_clat, g_zoom);
            SetCapture(hwnd);
        }
        return 0;
    }

    case WM_MOUSEMOVE:
        if (g_rp_drag) {
            RECT rc, br;
            GetClientRect(hwnd, &rc);
            replay_bar_rect(&rc, &br);
            replay_set_time(hwnd, replay_t_from_x(&br, MV_GET_X(lParam)));
            return 0;
        }
        if (g_split_drag) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            g_panel_w = clamp_panel_w(rc.right - MV_GET_X(lParam), rc.right);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (g_dragging) {
            int mx = MV_GET_X(lParam), my = MV_GET_Y(lParam);
            g_clon = wrap_lon(MV_LON(g_drag_wx - (mx - g_drag_x), g_zoom));
            g_clat = clamp_lat_view(MV_LAT(g_drag_wy - (my - g_drag_y), g_zoom));
            invalidate_view(hwnd);
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_rp_drag) {
            g_rp_drag = 0;
            ReleaseCapture();
            return 0;
        }
        if (g_split_drag) {
            g_split_drag = 0;
            ReleaseCapture();
            save_panel_w();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (g_dragging) {
            g_dragging = 0;
            ReleaseCapture();
        }
        return 0;

    case WM_LBUTTONDBLCLK: {
        int mx = MV_GET_X(lParam), my = MV_GET_Y(lParam);
        if (in_split_zone(hwnd, mx, my)) { /* двойной щелчок по разделителю - вернуть ширину по умолчанию */
            RECT rc;
            GetClientRect(hwnd, &rc);
            g_panel_w = clamp_panel_w(MAP_PANEL_W_DEFAULT, rc.right);
            save_panel_w();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        RECT m;
        map_rect(hwnd, &m);
        if (mx >= m.left && mx < m.right && my >= m.top && my < m.bottom) {
            set_zoom_at(&m, g_zoom + 1, mx, my);
            invalidate_view(hwnd);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        POINT pt;
        pt.x = MV_GET_X(lParam);
        pt.y = MV_GET_Y(lParam);
        ScreenToClient(hwnd, &pt);
        RECT m;
        map_rect(hwnd, &m);
        int steps = GET_WHEEL_DELTA_WPARAM(wParam) / 120;
        if (steps == 0)
            steps = GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1 : -1;
        if (pt.x >= m.right && pt.y >= MAP_TOOLBAR_H && g_nac > panel_rows_shown()) {
            /* колёсико над правой панелью - прокрутка списка бортов (вверх - к началу списка) */
            g_list_top -= steps;
            list_clamp_top();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (pt.x < m.left || pt.x >= m.right || pt.y < m.top || pt.y >= m.bottom) {
            pt.x = (m.left + m.right) / 2;
            pt.y = (m.top + m.bottom) / 2;
        }
        set_zoom_at(&m, g_zoom + steps, pt.x, pt.y);
        invalidate_view(hwnd);
        return 0;
    }

    case WM_KEYDOWN: {
        RECT m;
        map_rect(hwnd, &m);
        int cx = (m.left + m.right) / 2, cy = (m.top + m.bottom) / 2;
        switch (wParam) {
        case VK_ADD:
        case VK_OEM_PLUS:
            set_zoom_at(&m, g_zoom + 1, cx, cy);
            break;
        case VK_SUBTRACT:
        case VK_OEM_MINUS:
            set_zoom_at(&m, g_zoom - 1, cx, cy);
            break;
        case 'F':
            if (GetKeyState(VK_CONTROL) & 0x8000) { /* Ctrl+F - в поле поиска */
                SetFocus(g_edSearch);
                SendMessageW(g_edSearch, EM_SETSEL, 0, -1);
                return 0;
            }
            fit_view(hwnd);
            break;
        case VK_OEM_2: /* "/" - тоже поиск */
            SetFocus(g_edSearch);
            SendMessageW(g_edSearch, EM_SETSEL, 0, -1);
            return 0;
        case VK_UP:   /* стрелка вверх - предыдущая (более ранняя) точка следа выбранного борта; карту не двигает */
        case VK_DOWN: { /* стрелка вниз - следующая точка */
            int si = find_sel();
            if (si < 0 || g_ac[si].n_trail < 1)
                return 0;
            int cur = cur_point(&g_ac[si]);
            cur += (wParam == VK_UP) ? -1 : 1;
            if (cur < 0) cur = 0;
            if (cur > g_ac[si].n_trail - 1) cur = g_ac[si].n_trail - 1;
            g_sel_pt = cur;
            center_on_point(&g_ac[si], cur);
            break;
        }
        case 'T': /* проигрывание истории: вкл/выкл */
            replay_toggle(hwnd);
            return 0;
        case VK_SPACE:
            if (g_rp_on) {
                g_rp_play = !g_rp_play;
                if (g_rp_play) {
                    if (g_rp_t >= g_rp_max) g_rp_t = g_rp_min;
                    SetTimer(hwnd, TIMER_REPLAY, 250, NULL);
                } else
                    KillTimer(hwnd, TIMER_REPLAY);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case VK_LEFT:
        case VK_RIGHT:
            if (g_rp_on) {
                long step = (GetKeyState(VK_CONTROL) & 0x8000) ? 3600 : 600;
                replay_set_time(hwnd, g_rp_t + (wParam == VK_LEFT ? -step : step));
            }
            return 0;
        case 'A': /* аэропорты на карте: вкл/выкл */
            g_show_airports = !g_show_airports;
            break;
        case 'B': { /* все сообщения выбранного борта */
            int si = find_sel();
            if (si >= 0 && g_report_cb)
                g_report_cb(hwnd, g_ac[si].reg);
            return 0;
        }
        case 'I': /* подробная служебная информация о кэше: вкл/выкл */
            g_show_info = !g_show_info;
            break;
        case 'M': /* вид пустых мест: как есть -> приглушённо -> без картинки */
            g_fill_mode = (g_fill_mode + 2) % 3;
            drop_all_fallbacks();
            {
                WCHAR ini[MAX_PATH + 32], v[8];
                cwd_file(L"acars_settings.ini", ini);
                wsprintfW(v, L"%d", g_fill_mode);
                WritePrivateProfileStringW(L"Map", L"FillMode", v, ini);
            }
            break;
        default:
            return 0;
        }
        invalidate_view(hwnd);
        return 0;
    }

    case WM_APP_TILE: {
        int x = (int)(wParam & 0xFFFF);
        int z = (int)((wParam >> 16) & 0xFF);
        int ok = (int)((wParam >> 24) & 0x01);
        int st = (int)((wParam >> 25) & 0x0F);
        int y = (int)lParam;
        TileSlot *s = st < N_STYLES ? tile_find(st, z, x, y) : NULL;
        if (s) {
            if (ok) {
                s->state = TS_EMPTY; /* файл появился в кэше - при ближайшей отрисовке загрузится */
            } else {
                s->state = TS_FAILED;
                s->fail_time = GetTickCount();
            }
        }
        invalidate_view(hwnd);
        return 0;
    }

    case WM_APP_TILEDONE:
        tile_result_apply((TileResult *)lParam);
        invalidate_view(hwnd);
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_REPLAY) {
            if (!g_rp_on || !g_rp_play) {
                KillTimer(hwnd, TIMER_REPLAY);
                return 0;
            }
            long nt = g_rp_t + 600; /* 10 минут истории за 0.25 с */
            if (nt >= g_rp_max) {
                nt = g_rp_max;
                g_rp_play = 0;
                KillTimer(hwnd, TIMER_REPLAY);
            }
            replay_set_time(hwnd, nt);
            return 0;
        }
        if (wParam == TIMER_REFRESH && IsWindowVisible(hwnd)) {
            refresh_data();
            if (kStyles[g_style].local) {
                /* в папку кэша могли докопировать новый масштаб (z13...), пока карта открыта: следим */
                int lo = g_zmin_s[g_style], hi = g_zmax_s[g_style];
                sas_scan_zooms(g_style);
                if (lo != g_zmin_s[g_style] || hi != g_zmax_s[g_style])
                    clamp_zoom();
            }
            {
                int sel = find_sel();
                if (sel >= 0)
                    photo_request_for(&g_ac[sel], 0); /* сама ничего не делает, если фото уже есть/запрошено */
            }
            if (!g_dragging && !g_split_drag) /* во время сдвига панель не трогаем - иначе раз в 2 с кадр подвисал бы */
                InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_CLOSE:
        KillTimer(hwnd, TIMER_REFRESH);
        ShowWindow(hwnd, SW_HIDE); /* прячем, не уничтожаем - кэш тайлов в памяти остаётся */
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static HWND make_button(HWND parent, const WCHAR *text, int x, int w, int id, DWORD style) {
    HWND b = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | style, x, 7, w, 26, parent,
                           (HMENU)(INT_PTR)id, NULL, NULL);
    if (b)
        SendMessageW(b, WM_SETFONT, (WPARAM)g_font, TRUE);
    return b;
}

static int create_map_window(HWND parent) {
    GetCurrentDirectoryW(MAX_PATH - 20, g_cache_root);
    wcscat(g_cache_root, L"\\acars_tiles");
    InitializeCriticalSection(&g_qcs);
    g_qevt = CreateEventW(NULL, FALSE, FALSE, NULL);
    InitializeCriticalSection(&g_lqcs);
    g_lqevt = CreateEventW(NULL, FALSE, FALSE, NULL);
    gdip_init(); /* запускаем GDI+ здесь, в потоке окна, ДО фонового потока: так её не придётся запускать из двух мест сразу */

    g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, RUSSIAN_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                         xp_ui_face());
    g_fontBold = CreateFontW(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, RUSSIAN_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, xp_ui_face());
    g_fontSmall = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, RUSSIAN_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, xp_ui_face());
    g_brToolbar = CreateSolidBrush(CLR_BG);

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MapWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.hIcon = parent ? (HICON)GetClassLongPtrW(parent, GCLP_HICON) : NULL;
    wc.lpszClassName = L"AcarsMapWndClass";
    RegisterClassW(&wc);

    g_hwndMap = CreateWindowW(L"AcarsMapWndClass", L"Карта - самолёты",
                              WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                              1210, 740, parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_hwndMap)
        return 0;

    g_btnFit = make_button(g_hwndMap, L"Показать все", 8, 120, IDC_MAP_FIT, BS_FLAT);
    g_btnZin = make_button(g_hwndMap, L"+", 136, 36, IDC_MAP_ZIN, BS_FLAT);
    g_btnZout = make_button(g_hwndMap, L"\u2212", 178, 36, IDC_MAP_ZOUT, BS_FLAT);
    g_chkTrail = make_button(g_hwndMap, L"Следы", 228, 80, IDC_MAP_TRAIL, BS_AUTOCHECKBOX);
    SendMessageW(g_chkTrail, BM_SETCHECK, BST_CHECKED, 0);
    g_btnWeb = make_button(g_hwndMap, L"В браузере", 316, 140, IDC_MAP_WEB, BS_FLAT);

    for (int i = 0; i < N_STYLES; i++) {
        g_zmin_s[i] = g_zmax_s[i] = -1;
        if (kStyles[i].local) {
            load_sas_dir(i);
            if (g_localdir[i][0] && !sas_scan_zooms(i))
                g_localdir[i][0] = L'\0'; /* папку удалили/переместили - спросим заново при выборе */
        }
    }
    g_btnSas = make_button(g_hwndMap, L"Папка кэша...", 880, 130, IDC_MAP_SAS, BS_FLAT);
    g_btnOvr = make_button(g_hwndMap, L"Слой подписей...", 1018, 150, IDC_MAP_OVR, BS_FLAT);

    /* список стилей подложки; сохранённый выбор берём из acars_settings.ini, но стиль,
     * которому нужна папка кэша, возвращаем на стандартный, если её сейчас нет */
    g_cmbStyle = CreateWindowW(L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                               464, 7, 266, 220, g_hwndMap, (HMENU)(INT_PTR)IDC_MAP_STYLE, NULL, NULL);
    if (g_cmbStyle) {
        SendMessageW(g_cmbStyle, WM_SETFONT, (WPARAM)g_font, TRUE);
        for (int i = 0; i < N_STYLES; i++)
            SendMessageW(g_cmbStyle, CB_ADDSTRING, 0, (LPARAM)kStyles[i].name);
        WCHAR ini[MAX_PATH + 32];
        cwd_file(L"acars_settings.ini", ini);
        WCHAR sid[64];
        GetPrivateProfileStringW(L"Map", L"StyleId", L"", sid, 64, ini);
        int st = sid[0] ? style_index_from_id(sid) : -1;
        if (!sid[0]) /* настройки от прежней версии: стиль был записан номером */
            st = style_index_from_id(legacy_style_id((int)GetPrivateProfileIntW(L"Map", L"Style", 0, ini)));
        if (st < 0)
            st = 0;
        if (kStyles[st].local && !g_localdir[st][0])
            st = 0; /* папка кэша не задана или пропала - вернёмся на стандартную карту */
        g_style = st;
        clamp_zoom();
        update_style_ui();
        SendMessageW(g_cmbStyle, CB_SETCURSEL, (WPARAM)g_style, 0);
    }

    g_edSearch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                 738, 8, 134, 24, g_hwndMap, (HMENU)(INT_PTR)IDC_MAP_SEARCH, NULL, NULL);
    if (g_edSearch) {
        SendMessageW(g_edSearch, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(g_edSearch, EM_SETLIMITTEXT, 40, 0);
        SendMessageW(g_edSearch, EM_SETCUEBANNER, TRUE, (LPARAM)L"Поиск: борт, рейс...");
        g_origSearchProc = (WNDPROC)SetWindowLongPtrW(g_edSearch, GWLP_WNDPROC, (LONG_PTR)SearchEditProc);
    }

    g_worker = CreateThread(NULL, 0, tile_worker, NULL, 0, NULL);
    g_lworker = CreateThread(NULL, 0, tile_loader, NULL, 0, NULL);

    InitializeCriticalSection(&g_pcs);
    g_pevt = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_pworker = CreateThread(NULL, 0, photo_worker, NULL, 0, NULL);

    {
        WCHAR ini[MAX_PATH + 32];
        cwd_file(L"acars_settings.ini", ini);
        int fm = (int)GetPrivateProfileIntW(L"Map", L"FillMode", 2, ini);
        g_fill_mode = (fm >= 0 && fm <= 2) ? fm : 2;
        int w = (int)GetPrivateProfileIntW(L"Map", L"PanelWidth", MAP_PANEL_W_DEFAULT, ini);
        if (w < MAP_PANEL_W_MIN || w > 3000)
            w = MAP_PANEL_W_DEFAULT;
        g_panel_w = w; /* под реальный размер окна подгонится при показе (clamp_panel_w) */
    }
    return 1;
}

void mapview_show(HWND parent, MapProviderFn provider) {
    g_provider = provider;
    if (!g_hwndMap && !create_map_window(parent))
        return;

    ensure_photo_dir(); /* чтобы папка для своих фото была на месте с первого запуска карты */
    refresh_data();
    ShowWindow(g_hwndMap, IsIconic(g_hwndMap) ? SW_RESTORE : SW_SHOW);
    {
        RECT rc;
        GetClientRect(g_hwndMap, &rc);
        g_panel_w = clamp_panel_w(g_panel_w, rc.right);
    }
    fit_view(g_hwndMap); /* после показа - размеры клиентской области уже известны */
    SetTimer(g_hwndMap, TIMER_REFRESH, 2000, NULL);
    InvalidateRect(g_hwndMap, NULL, FALSE);
    SetForegroundWindow(g_hwndMap);
    SetFocus(g_hwndMap);
}
