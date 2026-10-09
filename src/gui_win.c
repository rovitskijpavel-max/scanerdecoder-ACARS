#ifdef _WIN32

/*
 * Простой графический интерфейс поверх той же самой логики, что и
 * консольный режим - никакой отдельной "GUI-версии" декодера не
 * существует, это тонкая обёртка над теми же run_live_capture() /
 * decode_wav_file(), что вызывает и консольный main.c.
 *
 * Технически - обычный, "голый" Win32 (CreateWindow/WndProc), без
 * сторонних библиотек: работает на всех версиях Windows от XP до 11 в
 * одном и том же .exe, как и остальная программа (см. README, раздел
 * "Поддержка Windows XP"). Используется "широкий" (W) вариант API
 * (RegisterClassW, CreateWindowW и т.п.) - это гарантирует корректное
 * отображение русского текста независимо от системной кодовой
 * страницы; доступен на всех NT-системах, включая XP (в отличие от
 * консоли, у окон Windows проблем с UTF-8/Unicode нет).
 *
 * Приём "живого" вывода программы (все существующие printf() в
 * decode_wav_file/run_live_capture) устроен без переписывания этих
 * функций: stdout процесса перенаправляется в анонимный канал (pipe),
 * а отдельный поток читает из него и построчно добавляет текст в окно
 * вывода. Так GUI сразу "видит" всё то же самое, что видел бы
 * пользователь в консоли - без необходимости дублировать вывод во
 * всех местах, где программа печатает сообщения.
 *
 * Отдельно, в дополнение к текстовому логу - табличный вид (ListView в
 * режиме "отчёт", с сортировкой по клику на заголовок колонки). Ради
 * него сообщения ТАКЖЕ приходят в структурированном виде через
 * msg_sink.c (см. msg_sink.h) - парсить обратно уже отформатированный
 * текст лога было бы хрупко.
 *
 * ВАЖНО: как и audio_win.c, этот файл не компилировался в текущей
 * сессии (здесь нет Windows-тулчейна) - собрать и обкатать нужно будет
 * на реальной машине. Код написан аккуратно и по стандартному,
 * десятилетиями стабильному Win32 API, но при первом запуске возможны
 * мелкие шероховатости (см. README).
 */

/* UNICODE/_UNICODE - чтобы "родовые" (без суффикса A/W) макросы
 * comctl32 (ListView_InsertItem и т.п.) и TEXT()/WC_LISTVIEW сами
 * резолвились в Unicode-варианты - используются наравне с уже везде
 * применяемыми явными *W-функциями. */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <mmsystem.h>
#include <commctrl.h>
#include <shellapi.h>
#include <commdlg.h>
#include <richedit.h>
#include <io.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <time.h>

#include "app.h"
#include "audio_win.h"
#include "labels.h"
#include "msg_sink.h"
#include "ar5000.h"
#include "aircraft_db.h"
#include "fileio.h"
#include <errno.h>
#include "airport_db.h"
#include "cfdiu_fault.h"
#include "asu_telemetry.h"
#include "acars_report.h"
#include "journal_export.h"
#include "aircraft_report.h"
#include "memory_view.h"
#include "units.h"
#include "xpcompat.h"
#include "metar.h"
#include "geomap.h"
#include "map_view.h"
#include "ooi_parse.h"
#include "resource.h"

#ifdef _MSC_VER
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")
#endif
/* Под MinGW comdlg32/comctl32 подключаются через -lcomdlg32 -lcomctl32
 * в Makefile/CMakeLists.txt. */

#define IDC_COMBO_DEVICE  101
#define IDC_BTN_REFRESH   102
#define IDC_BTN_START     103
#define IDC_BTN_FILE      104
#define IDC_BTN_CLEAR     105
#define IDC_EDIT_LOG      106
#define IDC_STATIC_STATUS 107
#define IDC_BTN_VIEW      108
#define IDC_TABLE         109
#define IDC_EDIT_COM        110
#define IDC_COMBO_BAUD      111
#define IDC_BTN_AR_CONNECT  112
#define IDC_EDIT_FREQ       113
#define IDC_BTN_TUNE        114
#define IDC_COMBO_PRESET    115
#define IDC_STATIC_AR_STATUS 116
#define IDC_BTN_SCAN        117
#define IDC_EDIT_FILTER     118
#define IDC_BTN_STATS       119
#define IDC_BTN_DIAG        120
#define IDC_CHECK_TOPMOST   121
#define IDC_CHECK_NOTIFY    122
#define IDC_COMBO_MODE      125
#define IDC_COMBO_BW        126
#define IDC_COMBO_MEMBANK   127
#define IDC_BTN_MEMLIST     128
#define IDC_BTN_ROUTES      129
#define IDC_BTN_FAULTS      130
#define IDC_BTN_TELEMETRY   131
#define IDC_BTN_WEATHER     132
#define IDC_BTN_DETAILS     133
#define IDC_CHECK_AUTOSTART 134
#define IDC_VIEWER_EDIT_FIND    135
#define IDC_VIEWER_BTN_FINDNEXT 136
#define IDC_BTN_MAP             137
#define IDC_BTN_IMPORT          138
#define IDC_BTN_EXPORT          139
#define IDC_EDIT_AIRCRAFT       140
#define IDM_TRAY_SHOW       123
#define IDM_TRAY_EXIT       124

#define WM_APP_LOG             (WM_APP + 1) /* lParam = malloc'd WCHAR*, GUI-поток обязан free() */
#define WM_APP_CAPTURE_STOPPED (WM_APP + 2)
#define WM_APP_FILE_DONE       (WM_APP + 3)
#define WM_APP_TABLE_ROW       (WM_APP + 4) /* lParam = malloc'd TableRow*, GUI-поток обязан free() */
#define WM_APP_AR_STATUS       (WM_APP + 5) /* wParam = уровень (0-255) или -1, lParam = 1 если шумоподавитель открыт */
#define WM_APP_TRAYICON         (WM_APP + 6) /* обратный вызов от значка в трее (Shell_NotifyIcon) */
#define WM_APP_AUTOLOAD         (WM_APP + 7) /* автозагрузка журнала сразу после создания окна */

/* ---------- тема оформления: тёмный "радарный экран" ---------- */
#define CLR_BG_MAIN    RGB(22, 26, 33)   /* фон окна - тёмно-синий графит */
#define CLR_BG_LOG     RGB(8, 12, 15)    /* фон окна вывода - почти чёрный */
#define CLR_BG_ALL     RGB(70, 58, 0)    /* фон остальных вхождений искомого слова (текущее ярче) */
#define CLR_BG_FOUND   RGB(120, 90, 0)   /* фон подсветки найденного слова в поиске - тёмно-жёлтый,
                                           * виден на тёмной теме независимо от того, какой control
                                           * сейчас в фокусе (обычное выделение OS этого не умеет) */
#define CLR_FG_DEFAULT RGB(150, 190, 170) /* обычный текст - бледно-фосфорный зелёный */
#define CLR_FG_DIM     RGB(90, 110, 105)  /* второстепенные строки (служебная информация) */
#define CLR_FG_OK      RGB(80, 230, 120)  /* CRC сошёлся */
#define CLR_FG_ERR     RGB(240, 90, 90)   /* CRC не сошёлся / ошибка */
#define CLR_FG_FIELD   RGB(110, 190, 250) /* поля сообщения (Mode/Addr/Label/...) */
#define CLR_FG_TEXT    RGB(255, 205, 90)  /* сам текст расшифрованного сообщения */
#define CLR_FG_LABEL   RGB(210, 220, 215) /* текст статичных подписей на тёмном фоне */

static HWND g_hwnd_main;
static HWND g_hwndTextViewer;
static HWND g_hEditTextViewer;
static HWND g_hEditFind;
static int g_viewerIsRichEdit;    /* 1, если удалось создать RichEdit (а не обычный EDIT-запасной
                                    * вариант) - от этого зависит, можем ли мы красить фон под
                                    * найденным словом независимо от фокуса (см. find_next_in_viewer) */
static int g_lastFoundPos = -1, g_lastFoundLen = 0; /* последнее подсвеченное место - чтобы
                                                       * снять с него подсветку перед тем, как
                                                       * подсветить новое */
static WCHAR g_lastSearchText[256]; /* что искали в прошлый раз - если пользователь поменял
                                      * текст в поле поиска, начинаем заново от начала, а не
                                      * с того места, где остановился ПРЕЖНИЙ, другой запрос */
static HWND g_hBtnFindNext;
static HWND g_hLabelFindCount;
static int g_matchIndex = 0;  /* номер текущей находки по счёту (1-based), 0 = ещё не искали */
static int g_matchTotal = 0;  /* всего совпадений для текущего текста в поле поиска */
static WNDPROC g_origFindEditProc;
static WNDPROC g_origViewerEditProc;
static HWND g_hCombo, g_hBtnStart, g_hBtnFile, g_hBtnRefresh, g_hBtnClear, g_hEditLog, g_hStatus;
static HWND g_hBtnView, g_hTable;
static HWND g_hEditCom, g_hComboBaud, g_hBtnArConnect, g_hEditFreq, g_hBtnTune, g_hComboPreset, g_hArStatus;
static HWND g_hBtnScan;
static HWND g_hBtnStats;
static HWND g_hBtnRoutes;
static HWND g_hBtnImport;
static HWND g_hBtnExport;
static HWND g_hBtnFaults;
static HWND g_hBtnTelemetry;
static HWND g_hBtnWeather;
static HWND g_hBtnDetails;
static HWND g_hBtnMap;
static HWND g_hCheckAutostart;
static HWND g_hBtnDiag;
static HWND g_hCheckTopmost;
static HWND g_hCheckNotify;
static HWND g_hComboMode, g_hComboBW;
static HWND g_hComboMemBank, g_hBtnMemList;
static NOTIFYICONDATAW g_nid;
static int g_tray_added = 0;
static int g_shown_tray_hint = 0;
static HWND g_hArProgress;
static HBRUSH g_hbrMainBg;
static HBRUSH g_hbrLogBg;
static COLORREF g_status_color = CLR_FG_LABEL;
static COLORREF g_ar_status_color = CLR_FG_DIM;
static int g_table_view = 0; /* 0 = текстовый лог виден, 1 = таблица виден */
static HWND g_hEditFilter;
static HWND g_hEditAircraft; /* фильтр таблицы по борту */
static int g_sort_column = -1;
static int g_sort_ascending = TRUE;

/* Частые частоты ACARS (Европа + США) - см. README, раздел
 * "Подключение AR5000". Выбор в выпадающем списке подставляет частоту
 * в поле ввода и сразу настраивает приёмник, если он подключён. */
static const double kAcarsPresets[] = {130.025, 131.550, 131.725, 131.825, 136.750, 136.900, 136.975};
#define N_ACARS_PRESETS (int)(sizeof(kAcarsPresets) / sizeof(kAcarsPresets[0]))

static volatile int g_ar_poll_stop = 0;
static HANDLE g_ar_poll_thread = NULL;

static volatile int g_scan_active = 0;
static volatile int g_scan_stop = 0;
static HANDLE g_scan_thread = NULL;
static double g_scan_current_mhz = 0.0; /* читается/пишется только из потока сканера
                                          * и читается из GUI-потока для отображения -
                                          * небольшая гонка при чтении в худшем случае
                                          * даёт на кадр устаревшее значение, не более. */

static volatile int g_stop_flag = 0;
static HANDLE g_capture_thread = NULL;
static int g_capturing = 0;
static int g_file_decoding = 0;

static HANDLE g_stdout_read = NULL;

/* ---------- перекодировка строк для отображения в окне ---------- */

/* Наши собственные строки (то, что приходит через перенаправленный
 * stdout - все printf() в decode_wav_file/run_live_capture) - в
 * исходниках в UTF-8. */
static WCHAR *utf8_to_wide(const char *s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (len <= 0) return NULL;
    WCHAR *w = (WCHAR *)malloc((size_t)len * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, len);
    return w;
}

/* Обратное преобразование - нужно, когда исходный (узкий, UTF-8) текст
 * сообщения уже потерян, а есть только его widened-копия из TableRow
 * (см. show_routes ниже - там нужно скормить текст сообщения
 * ooi_parse()/airport_db_scan_pair(), которые работают с char*, а не
 * с WCHAR*). out должен быть достаточно большим (up to 4x от числа
 * широких символов с запасом). */
static void wide_to_utf8(const WCHAR *w, char *out, size_t out_size) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)out_size, NULL, NULL);
    if (n <= 0 && out_size)
        out[0] = '\0';
}

/* Простое безопасное копирование строки с гарантированным
 * завершающим нулём - используется в show_routes() ниже, где нужно
 * копировать много маленьких char-полей (коды ICAO, названия городов)
 * без риска переполнения буфера. */
static void safe_copy_local(char *dst, size_t dst_size, const char *src) {
    size_t i = 0;
    for (; i < dst_size - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Идентификатор борта из поля addr строки таблицы. Само поле addr бывает
 * длинным: если загружена база бортов (aircraft_db.csv), там лежит не
 * просто ".VP-BSY", а ".VP-BSY — Airbus A321 (A321), Оператор". Для
 * группировки "все сообщения одного борта" нужна только первая часть -
 * регистрация (до пробела/перевода строки). Раньше отчёты копировали
 * первые 15 символов длинной строки, а сравнивали потом с ПОЛНОЙ - и для
 * борта с известным типом "тот же борт" никогда не совпадал. */
static void reg_key(const WCHAR *addr, WCHAR out[16]) {
    int n = 0;
    while (addr[n] && addr[n] != L' ' && addr[n] != L'\r' && addr[n] != L'\n' && n < 15) {
        out[n] = addr[n];
        n++;
    }
    out[n] = L'\0';
}

static int reg_matches(const WCHAR *key, const WCHAR *row_addr) {
    WCHAR k[16];
    reg_key(row_addr, k);
    return wcscmp(key, k) == 0;
}

/* Имена устройств из WAVEINCAPSA.szPname приходят в СИСТЕМНОЙ (ANSI)
 * кодовой странице, а не в UTF-8 - их нужно перекодировать иначе. */
static WCHAR *ansi_to_wide(const char *s) {
    int len = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    if (len <= 0) return NULL;
    WCHAR *w = (WCHAR *)malloc((size_t)len * sizeof(WCHAR));
    MultiByteToWideChar(CP_ACP, 0, s, -1, w, len);
    return w;
}

/* ---------- табличный вид (ListView) ---------- */

typedef struct {
    WCHAR time[32];
    WCHAR source[160];
    WCHAR mode[2];
    WCHAR addr[220]; /* регистрация + опционально тип/оператор из aircraft_db.csv, см. gui_on_message */
    WCHAR label[4];
    WCHAR label_desc[300];
    WCHAR text[540];
} TableRow;

/* Постоянное хранилище ВСЕХ полученных сообщений (не только тех, что
 * сейчас показаны в таблице) - нужно, чтобы фильтр по тексту поиска
 * мог "показать" заново скрытые строки, а не только прятать видимые:
 * ListView (в отличие, например, от RichEdit-лога) не умеет скрывать
 * отдельные строки, поэтому при смене текста фильтра таблица просто
 * перестраивается заново из этого хранилища (см. rebuild_table_view).
 * Ограничено MAX_STORED_ROWS - при превышении старые записи вытесняются
 * новыми (простая FIFO-политика), чтобы не расти безгранично в долгой
 * сессии живого приёма. */
#define MAX_STORED_ROWS 5000
static TableRow *g_all_rows = NULL;
static int g_all_rows_count = 0;
static int g_all_rows_cap = 0;

static void store_row(const TableRow *row) {
    if (g_all_rows_count >= MAX_STORED_ROWS) {
        /* Вытесняем самые старые записи ПАЧКОЙ - десятую часть сразу, а не по одной. Сдвиг массива - это
         * перенос всех 5000 записей (около 12 МБ); по одной записи это делалось бы на КАЖДОЕ новое сообщение,
         * и загрузка большого журнала превращалась в десятки секунд зависания. Пачкой - один сдвиг на
         * MAX_STORED_ROWS/10 сообщений. В памяти остаётся от 90% до 100% потолка последних сообщений. */
        int drop = MAX_STORED_ROWS / 10;
        memmove(g_all_rows, g_all_rows + drop, (size_t)(g_all_rows_count - drop) * sizeof(TableRow));
        g_all_rows_count -= drop;
    }
    if (g_all_rows_count >= g_all_rows_cap) {
        int new_cap = g_all_rows_cap == 0 ? 256 : g_all_rows_cap * 2;
        if (new_cap > MAX_STORED_ROWS)
            new_cap = MAX_STORED_ROWS;
        TableRow *new_arr = (TableRow *)realloc(g_all_rows, (size_t)new_cap * sizeof(TableRow));
        if (!new_arr)
            return; /* нехватка памяти - просто не сохраняем эту запись, не падаем */
        g_all_rows = new_arr;
        g_all_rows_cap = new_cap;
    }
    g_all_rows[g_all_rows_count++] = *row;
}

/* Регистронезависимый поиск подстроки в широкой строке. */
static int wcs_ci_contains(const WCHAR *haystack, const WCHAR *needle) {
    if (!needle || !*needle)
        return 1; /* пустой фильтр - совпадает со всем */
    size_t hn = wcslen(haystack), nn = wcslen(needle);
    if (nn > hn)
        return 0;
    for (size_t i = 0; i + nn <= hn; i++) {
        size_t j = 0;
        for (; j < nn; j++) {
            if (towlower((wint_t)haystack[i + j]) != towlower((wint_t)needle[j]))
                break;
        }
        if (j == nn)
            return 1;
    }
    return 0;
}

/* Копия строки без '.', '-' и пробелов, в нижнем регистре: ".RA-73704", "ra73704" и "RA 73704" - один борт. */
static void norm_reg_w(WCHAR *dst, size_t cap, const WCHAR *src) {
    size_t j = 0;
    for (; *src && j + 1 < cap; src++) {
        if (*src == L'.' || *src == L'-' || *src == L' ')
            continue;
        dst[j++] = (WCHAR)towlower((wint_t)*src);
    }
    dst[j] = L'\0';
}

/* Фильтр "Борт": подходит, если запрос входит в регистрацию строки (поле addr); пустой запрос подходит всем. */
static int row_matches_aircraft(const TableRow *row, const WCHAR *needle_norm) {
    if (!needle_norm || !*needle_norm)
        return 1;
    WCHAR a[240];
    norm_reg_w(a, 240, row->addr);
    return wcsstr(a, needle_norm) != NULL;
}

static void read_aircraft_filter(WCHAR *out, size_t cap) {
    WCHAR raw[64] = L"";
    if (g_hEditAircraft)
        GetWindowTextW(g_hEditAircraft, raw, 64);
    norm_reg_w(out, cap, raw);
}

/* Строка подходит под фильтр, если текст поиска встречается хоть в
 * одном из показываемых в таблице полей (без учёта регистра). */
static int row_matches_filter(const TableRow *row, const WCHAR *filter) {
    if (!filter || !*filter)
        return 1;
    return wcs_ci_contains(row->time, filter) || wcs_ci_contains(row->source, filter) ||
           wcs_ci_contains(row->mode, filter) || wcs_ci_contains(row->addr, filter) ||
           wcs_ci_contains(row->label, filter) || wcs_ci_contains(row->label_desc, filter) ||
           wcs_ci_contains(row->text, filter);
}

/* wsprintfW НЕ поддерживает спецификатор %f вовсе (это давняя,
 * задокументированная особенность этой облегчённой функции из user32 -
 * в отличие от полноценного printf из CRT) - "%.3f" молча даёт мусор
 * вместо числа. Форматируем частоту (МГц, всегда 3 знака после
 * запятой) вручную через целочисленную арифметику и только те
 * спецификаторы (%ld), что wsprintfW действительно поддерживает. */
static void format_mhz(WCHAR *out, size_t out_size, double mhz) {
    long whole = (long)mhz;
    long frac = (long)((mhz - (double)whole) * 1000.0 + 0.5); /* округление до мсек. МГц */
    if (frac >= 1000) {
        frac -= 1000;
        whole += 1;
    }
    WCHAR tmp[32];
    wsprintfW(tmp, L"%ld.%03ld", whole, frac);
    wcsncpy(out, tmp, out_size - 1);
    out[out_size - 1] = L'\0';
}

static void widen_into(WCHAR *dst, size_t dst_count, const char *src) {
    WCHAR *w = utf8_to_wide(src ? src : "");
    if (w) {
        wcsncpy(dst, w, dst_count - 1);
        dst[dst_count - 1] = L'\0';
        free(w);
    } else {
        dst[0] = L'\0';
    }
}

/* Вызывается из РАБОЧЕГО потока (живой приём или декодирование файла) -
 * через msg_sink.c, см. msg_sink_register() в run_gui(). Сюда попадают
 * ТОЛЬКО достоверные (crc_ok) сообщения - это гарантирует framesync.c/
 * main.c до вызова msg_sink_notify(). Структуру сообщения копируем на
 * куче и передаём указателем в GUI-поток через PostMessage - сам GUI-
 * поток обязан free() после того, как заберёт данные (см. обработчик
 * WM_APP_TABLE_ROW). */
static void gui_on_message(const AcarsMessage *m, const char *source) {
    TableRow *row = (TableRow *)malloc(sizeof(TableRow));
    if (!row)
        return;

    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    char timestr[32];
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", tm_now);
    widen_into(row->time, 32, timestr);

    widen_into(row->source, 160, source);

    char modebuf[2] = {m->mode ? m->mode : '?', '\0'};
    widen_into(row->mode, 2, modebuf);

    widen_into(row->addr, 220, m->addr[0] ? m->addr : "(?)");
    {
        AircraftInfo ac;
        if (m->addr[0] && aircraft_db_lookup(m->addr, &ac)) {
            char addr_buf[300];
            if (ac.operator_name[0])
                snprintf(addr_buf, sizeof(addr_buf), "%s \xe2\x80\x94 %s %s%s%s%s, %s", m->addr,
                         ac.manufacturer, ac.model,
                         ac.typecode[0] ? " (" : "", ac.typecode, ac.typecode[0] ? ")" : "",
                         ac.operator_name);
            else
                snprintf(addr_buf, sizeof(addr_buf), "%s \xe2\x80\x94 %s %s%s%s%s", m->addr,
                         ac.manufacturer, ac.model,
                         ac.typecode[0] ? " (" : "", ac.typecode, ac.typecode[0] ? ")" : "");
            widen_into(row->addr, 220, addr_buf);
        }
    }

    char labelbuf[3] = {m->label[0], m->label[1], '\0'};
    widen_into(row->label, 4, labelbuf);

    widen_into(row->label_desc, 300, acars_label_lookup(m->label));

    widen_into(row->text, 540, m->has_text ? m->text : "");

    PostMessageW(g_hwnd_main, WM_APP_TABLE_ROW, 0, (LPARAM)row);
}

static void table_add_row(const TableRow *row) {
    int idx = ListView_GetItemCount(g_hTable);
    LVITEMW lvi;
    memset(&lvi, 0, sizeof(lvi));
    lvi.mask = LVIF_TEXT;
    lvi.iItem = idx;
    lvi.iSubItem = 0;
    lvi.pszText = (LPWSTR)row->time;
    ListView_InsertItem(g_hTable, &lvi);
    ListView_SetItemText(g_hTable, idx, 1, (LPWSTR)row->source);
    ListView_SetItemText(g_hTable, idx, 2, (LPWSTR)row->mode);
    ListView_SetItemText(g_hTable, idx, 3, (LPWSTR)row->addr);
    ListView_SetItemText(g_hTable, idx, 4, (LPWSTR)row->label);
    ListView_SetItemText(g_hTable, idx, 5, (LPWSTR)row->label_desc);
    ListView_SetItemText(g_hTable, idx, 6, (LPWSTR)row->text);
    ListView_EnsureVisible(g_hTable, idx, FALSE);
}

/* Перестраивает видимое содержимое таблицы с нуля из постоянного
 * хранилища (g_all_rows), оставляя только строки, подходящие под
 * текущий текст в поле поиска. Вызывается при каждом изменении текста
 * фильтра (см. EN_CHANGE у IDC_EDIT_FILTER) - ListView не умеет
 * скрывать отдельные строки, только полностью пересобирать видимый
 * список. */
static void rebuild_table_view(void) {
    if (!g_hTable)
        return;
    WCHAR filter[256];
    GetWindowTextW(g_hEditFilter, filter, 256);
    WCHAR ac[64];
    read_aircraft_filter(ac, 64);
    ListView_DeleteAllItems(g_hTable);
    for (int i = 0; i < g_all_rows_count; i++) {
        if (row_matches_filter(&g_all_rows[i], filter) && row_matches_aircraft(&g_all_rows[i], ac))
            table_add_row(&g_all_rows[i]);
    }
}

/* ---------- статистика сессии ---------- */

#define MAX_STAT_KEYS 512

typedef struct {
    WCHAR key[64];
    int count;
} StatEntry;

/* Ищет key среди уже накопленных записей; если нашёл - увеличивает
 * счётчик, если нет и есть место - заводит новую запись со счётчиком 1.
 * Простой линейный поиск - при <= 5000 сообщениях и обычно не более
 * нескольких сотен уникальных меток/бортов это происходит мгновенно,
 * усложнять до хэш-таблицы смысла нет. */
static void stat_add(StatEntry *entries, int *n, const WCHAR *key) {
    for (int i = 0; i < *n; i++) {
        if (wcscmp(entries[i].key, key) == 0) {
            entries[i].count++;
            return;
        }
    }
    if (*n < MAX_STAT_KEYS) {
        wcsncpy(entries[*n].key, key, 63);
        entries[*n].key[63] = L'\0';
        entries[*n].count = 1;
        (*n)++;
    }
}

static int stat_compare_desc(const void *a, const void *b) {
    return ((const StatEntry *)b)->count - ((const StatEntry *)a)->count;
}

/* ---------- значок в системном трее и уведомления ---------- */

/* Добавляет значок в трей (использует уже загруженный значок
 * приложения hIcon, тот же, что и у самого окна/панели задач) с
 * подсказкой и обратным вызовом через WM_APP_TRAYICON. Вызывается один
 * раз при старте, после того как окно уже создано (нужен валидный
 * hwnd). Безопасно, если по какой-то причине не получится (редкость -
 * explorer.exe не запущен и т.п.) - просто не будет значка, остальная
 * программа продолжит работать как обычно. */
static void tray_add(HWND hwnd, HICON hIcon) {
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAYICON;
    g_nid.hIcon = hIcon;
    wcsncpy(g_nid.szTip, L"ACARS-декодер для AOR AR5000", (sizeof(g_nid.szTip) / sizeof(WCHAR)) - 1);
    g_tray_added = Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void tray_remove(void) {
    if (g_tray_added) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_tray_added = 0;
    }
}

/* Всплывающая подсказка из трея (не MessageBox - не перехватывает
 * фокус и не мешает работе, сама скрывается через несколько секунд,
 * поведение/вид зависит от настроек пользователя в Windows). title и
 * text обрезаются по размеру полей NOTIFYICONDATA, если длиннее -
 * это ограничение самого API, не наша забота дополнительно проверять. */
static void tray_balloon(const WCHAR *title, const WCHAR *text) {
    if (!g_tray_added)
        return;
    g_nid.uFlags = NIF_INFO;
    wcsncpy(g_nid.szInfoTitle, title, (sizeof(g_nid.szInfoTitle) / sizeof(WCHAR)) - 1);
    wcsncpy(g_nid.szInfo, text, (sizeof(g_nid.szInfo) / sizeof(WCHAR)) - 1);
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; /* вернуть обратно для последующих NIM_MODIFY */
}

/* Вызывается при каждом новом достоверном сообщении (из
 * WM_APP_TABLE_ROW) - если включены уведомления (флажок), звуковой
 * сигнал + всплывающая подсказка из трея с кратким содержанием. Звук -
 * через системный "Asterisk" (тот же, что Windows использует для
 * обычных информационных уведомлений) - уважает тему звуков
 * пользователя, а не свой собственный wav-файл. */
static void notify_new_message(const TableRow *row) {
    if (SendMessageW(g_hCheckNotify, BM_GETCHECK, 0, 0) != BST_CHECKED)
        return;

    PlaySoundW(L"SystemAsterisk", NULL, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);

    WCHAR text[256];
    wsprintfW(text, L"%s\r\n%s: %s", row->addr, row->label, row->label_desc);
    tray_balloon(L"Новое сообщение ACARS", text);
}

/* --- Прокручиваемое окно для длинных отчётов ---
 * MessageBoxW не умеет прокручиваться и плохо справляется с большим
 * объёмом текста (может просто не поместиться на экране или обрезаться)
 * - все отчёты, которые могут вырасти (Подробности/Телеметрия/Маршруты/
 * Неисправности/Погода/Статистика/Диагностика/Список памяти), вместо
 * него открываются в этом отдельном окне с обычной вертикальной
 * прокруткой. Окно одно на всё приложение, переиспользуется - повторное
 * открытие просто меняет заголовок и текст, не плодя новые окна; закрытие
 * (крестик) только прячет его (WM_CLOSE -> SW_HIDE), а не уничтожает,
 * чтобы не пересоздавать RichEdit-контрол каждый раз. */
/* Снимает подсветку с предыдущего найденного места (возвращает его
 * фон к обычному цвету текста окна) - вызывается перед тем, как
 * подсветить новое, и один раз при открытии нового отчёта. */
static int g_hlAllActive; /* 1 - в тексте подсвечены все вхождения запроса (тусклым фоном) */

static void set_back_range(int a, int b, COLORREF c) {
    SendMessageW(g_hEditTextViewer, EM_SETSEL, (WPARAM)a, (LPARAM)b);
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_BACKCOLOR;
    cf.crBackColor = c;
    SendMessageW(g_hEditTextViewer, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}

static void clear_found_highlight(void) {
    if (!g_viewerIsRichEdit || g_lastFoundPos < 0 || !g_hEditTextViewer)
        return;
    set_back_range(g_lastFoundPos, g_lastFoundPos + g_lastFoundLen, g_hlAllActive ? CLR_BG_ALL : CLR_BG_LOG);
    g_lastFoundPos = -1;
    g_lastFoundLen = 0;
}

/* Снимает подсветку всех вхождений со всего текста. */
static void clear_all_highlights(void) {
    if (!g_viewerIsRichEdit || !g_hEditTextViewer)
        return;
    if (g_hlAllActive) {
        SendMessageW(g_hEditTextViewer, WM_SETREDRAW, FALSE, 0);
        set_back_range(0, -1, CLR_BG_LOG);
        SendMessageW(g_hEditTextViewer, EM_SETSEL, 0, 0);
        SendMessageW(g_hEditTextViewer, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_hEditTextViewer, NULL, TRUE);
    }
    g_hlAllActive = 0;
    g_lastFoundPos = -1;
    g_lastFoundLen = 0;
}

/* Подсвечивает ВСЕ вхождения тусклым фоном - чтобы при прокрутке сразу было видно, где ещё есть искомое
 * (не более 2000 вхождений - дальше подсветка только замедляла бы окно). */
static void highlight_all_matches(const WCHAR *needle) {
    if (!g_viewerIsRichEdit || !g_hEditTextViewer)
        return;
    int nlen = (int)wcslen(needle);
    SendMessageW(g_hEditTextViewer, WM_SETREDRAW, FALSE, 0);
    if (g_hlAllActive)
        set_back_range(0, -1, CLR_BG_LOG);
    FINDTEXTW ft;
    ft.lpstrText = (LPWSTR)needle;
    int from = 0, cnt = 0;
    while (cnt < 2000) {
        ft.chrg.cpMin = from;
        ft.chrg.cpMax = -1;
        int pos = (int)SendMessageW(g_hEditTextViewer, EM_FINDTEXTW, FR_DOWN, (LPARAM)&ft);
        if (pos < 0)
            break;
        set_back_range(pos, pos + nlen, CLR_BG_ALL);
        from = pos + nlen;
        cnt++;
    }
    SendMessageW(g_hEditTextViewer, EM_SETSEL, 0, 0);
    SendMessageW(g_hEditTextViewer, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hEditTextViewer, NULL, TRUE);
    g_hlAllActive = 1;
    g_lastFoundPos = -1;
    g_lastFoundLen = 0;
}

/* Прокручивает окно отчёта так, чтобы символ pos оказался примерно посередине видимой области ("камера
 * наводится на слово"). EM_SCROLLCARET для окна без фокуса (а фокус стоит в поле поиска) не прокручивал -
 * приходилось листать вручную; поэтому смещение считаем сами по строкам. */
static void center_viewer_on(int pos) {
    HWND h = g_hEditTextViewer;
    if (!h)
        return;
    int line = (int)SendMessageW(h, EM_LINEFROMCHAR, (WPARAM)pos, 0);
    int first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    RECT rc;
    GetClientRect(h, &rc);
    int lineh = 18;
    HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    HDC dc = GetDC(h);
    if (dc) {
        HGDIOBJ old = f ? SelectObject(dc, f) : NULL;
        TEXTMETRICW tm;
        if (GetTextMetricsW(dc, &tm) && tm.tmHeight > 0)
            lineh = tm.tmHeight + tm.tmExternalLeading;
        if (old)
            SelectObject(dc, old);
        ReleaseDC(h, dc);
    }
    if (lineh < 8)
        lineh = 8;
    int vis = (rc.bottom - rc.top) / lineh;
    if (vis < 1)
        vis = 1;
    int target = line - vis / 2;
    if (target < 0)
        target = 0;
    if (target != first)
        SendMessageW(h, EM_LINESCROLL, 0, (LPARAM)(target - first));
}

/* Считает, сколько раз needle встречается в содержимом окна-отчёта -
 * регистронезависимо, без пересечений. Один и тот же простой проход
 * (через свою копию текста + wcsstr) годится что для RichEdit, что
 * для обычного EDIT - в отличие от поиска ТОЧНОЙ ПОЗИЦИИ для подсветки
 * (см. комментарий у find_next_in_viewer про "\r\n"), для простого
 * ПОДСЧЁТА числа вхождений разница в том, как RichEdit считает
 * переводы строк, роли не играет - подсчитываемое слово через перевод
 * строки всё равно не бывает. */
static int count_occurrences(const WCHAR *needle) {
    if (!g_hEditTextViewer || !needle[0])
        return 0;
    int textLen = GetWindowTextLengthW(g_hEditTextViewer);
    if (textLen <= 0)
        return 0;
    WCHAR *full = (WCHAR *)malloc(sizeof(WCHAR) * (size_t)(textLen + 1));
    if (!full)
        return 0;
    GetWindowTextW(g_hEditTextViewer, full, textLen + 1);
    for (int i = 0; i <= textLen; i++)
        full[i] = (WCHAR)towlower(full[i]);

    WCHAR needle_lower[256];
    int nlen = 0;
    for (; needle[nlen] && nlen < 255; nlen++)
        needle_lower[nlen] = (WCHAR)towlower(needle[nlen]);
    needle_lower[nlen] = L'\0';

    int count = 0;
    WCHAR *p = full;
    while ((p = wcsstr(p, needle_lower)) != NULL) {
        count++;
        p += nlen;
    }
    free(full);
    return count;
}

/* Обновляет подпись "N из M" / "ничего не найдено" рядом с кнопкой
 * поиска. */
static void update_find_count_label(void) {
    if (!g_hLabelFindCount)
        return;
    WCHAR buf[64];
    if (g_matchTotal <= 0)
        wsprintfW(buf, L"ничего не найдено");
    else
        wsprintfW(buf, L"%d из %d", g_matchIndex, g_matchTotal);
    SetWindowTextW(g_hLabelFindCount, buf);
}

/* Ищет следующее (после предыдущей находки, с переносом по кругу в
 * начало, если дошли до конца) вхождение текста из поля поиска.
 * Регистронезависимо.
 *
 * ВАЖНО: для RichEdit позиции символов ищутся через её же собственное
 * сообщение EM_FINDTEXTW, а не вручную (GetWindowTextW в свой буфер +
 * wcsstr), как было раньше. Причина: RichEdit у переводов строк "\r\n"
 * внутри себя может хранить/считать их не так, как обычный текст (в
 * некоторых версиях сворачивает "\r\n" в один "\r") - из-за этого
 * позиция, посчитанная по СВОЕЙ копии текста, расходилась с тем, что
 * реально ожидает EM_SETSEL, и подсвечивалось случайное соседнее
 * место, а не то, что искали. EM_FINDTEXTW сама живёт в системе
 * координат RichEdit - расхождения в принципе быть не может. Для
 * обычного EDIT (запасной вариант, если RichEdit не загрузился) такой
 * проблемы с "\r\n" нет - там используется прежний способ. */
static void find_next_in_viewer_ex(int quiet) {
    if (!g_hEditTextViewer || !g_hEditFind)
        return;

    WCHAR needle[256];
    GetWindowTextW(g_hEditFind, needle, 256);
    if (!needle[0]) {
        clear_all_highlights();
        g_lastSearchText[0] = L'\0';
        g_matchIndex = g_matchTotal = 0;
        if (g_hLabelFindCount)
            SetWindowTextW(g_hLabelFindCount, L"");
        return;
    }
    int nlen = (int)wcslen(needle);

    /* ВАЖНО: считать точку начала поиска НУЖНО ДО clear_found_highlight() -
     * та в числе прочего сбрасывает g_lastFoundPos в -1 (чтобы было
     * видно, что подсветки сейчас нет), и если прочитать его ПОСЛЕ
     * сброса, поиск каждый раз будет начинаться заново от начала
     * текста и находить одно и то же первое совпадение - ровно то,
     * на что ты и обратил внимание. */
    int startFrom = g_lastFoundPos >= 0 ? g_lastFoundPos + g_lastFoundLen : 0;

    /* Если текст в поле поиска поменялся с прошлого раза - предыдущая
     * позиция относится к ДРУГОМУ запросу и продолжать поиск от нее
     * не имеет смысла (можно пропустить более раннее совпадение
     * нового запроса) - начинаем с начала. Заодно это и есть момент,
     * когда нужно пересчитать ОБЩЕЕ число совпадений - оно же зависит
     * именно от текста запроса. */
    int isNewSearch = wcscmp(needle, g_lastSearchText) != 0;
    if (isNewSearch) {
        startFrom = 0;
        wcsncpy(g_lastSearchText, needle, 255);
        g_lastSearchText[255] = L'\0';
        g_matchTotal = count_occurrences(needle);
        highlight_all_matches(needle); /* все вхождения сразу видны в тексте, текущее - ярче */
    }

    clear_found_highlight();
    int pos = -1;
    int wrapped = 0;

    if (g_viewerIsRichEdit) {
        FINDTEXTW ft;
        ft.chrg.cpMin = startFrom;
        ft.chrg.cpMax = -1; /* до самого конца текста */
        ft.lpstrText = needle;
        /* флаг FR_MATCHCASE НЕ передаём - без него поиск регистронезависимый,
         * это поведение EM_FINDTEXTW "из коробки", велосипед не нужен */
        pos = (int)SendMessageW(g_hEditTextViewer, EM_FINDTEXTW, FR_DOWN, (LPARAM)&ft);
        if (pos < 0) {
            /* не нашли дальше по тексту - ищем заново с самого начала (по кругу) */
            ft.chrg.cpMin = 0;
            ft.chrg.cpMax = -1;
            pos = (int)SendMessageW(g_hEditTextViewer, EM_FINDTEXTW, FR_DOWN, (LPARAM)&ft);
            wrapped = (pos >= 0);
        }
    } else {
        int textLen = GetWindowTextLengthW(g_hEditTextViewer);
        if (textLen > 0) {
            WCHAR *full = (WCHAR *)malloc(sizeof(WCHAR) * (size_t)(textLen + 1));
            if (full) {
                GetWindowTextW(g_hEditTextViewer, full, textLen + 1);
                int searchFrom = startFrom;
                for (int i = 0; i <= textLen; i++)
                    full[i] = (WCHAR)towlower(full[i]);
                WCHAR needle_lower[256];
                int k = 0;
                for (; needle[k]; k++)
                    needle_lower[k] = (WCHAR)towlower(needle[k]);
                needle_lower[k] = L'\0';

                WCHAR *found = searchFrom < textLen ? wcsstr(full + searchFrom, needle_lower) : NULL;
                if (!found) {
                    found = wcsstr(full, needle_lower);
                    wrapped = (found != NULL);
                }
                if (found)
                    pos = (int)(found - full);
                free(full);
            }
        }
    }

    if (pos >= 0) {
        SendMessageW(g_hEditTextViewer, EM_SETSEL, (WPARAM)pos, (LPARAM)(pos + nlen));
        SendMessageW(g_hEditTextViewer, EM_SCROLLCARET, 0, 0);
        center_viewer_on(pos);
        if (g_viewerIsRichEdit) {
            CHARFORMAT2W cf;
            memset(&cf, 0, sizeof(cf));
            cf.cbSize = sizeof(cf);
            cf.dwMask = CFM_BACKCOLOR;
            cf.crBackColor = CLR_BG_FOUND;
            SendMessageW(g_hEditTextViewer, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
            g_lastFoundPos = pos;
            g_lastFoundLen = nlen;
        }
        /* Номер текущей находки по счёту: 1, если это новый запрос или
         * поиск только что перенёсся по кругу в начало; иначе - просто
         * следующий по порядку. */
        g_matchIndex = (isNewSearch || wrapped) ? 1 : g_matchIndex + 1;
        SetFocus(g_hEditFind); /* Enter снова ищет ДАЛЬШЕ, не уходя из поля поиска */
    } else {
        g_matchIndex = 0;
        g_matchTotal = 0;
        if (!quiet)
            MessageBeep(MB_ICONEXCLAMATION); /* ничего не найдено, даже с переносом по кругу */
    }
    update_find_count_label();
}

static void find_next_in_viewer(void) { find_next_in_viewer_ex(0); }

/* Подменяем оконную процедуру поля поиска, чтобы поймать: Enter -
 * обычный WS_CHILD EDIT (не в диалоге) сам по себе не умеет сообщать
 * "нажали Enter" через WM_COMMAND, в отличие от кнопок; и Ctrl+A -
 * выделить весь текст в самом ПОЛЕ ПОИСКА (а не в окне отчёта) - тоже
 * не работает "из коробки" у обычного WS_CHILD EDIT без явной
 * обработки. */
static LRESULT CALLBACK FindEditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        find_next_in_viewer();
        return 0;
    }
    if (msg == WM_KEYDOWN && wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        SendMessageW(hwnd, EM_SETSEL, 0, -1);
        return 0;
    }
    return CallWindowProcW(g_origFindEditProc, hwnd, msg, wParam, lParam);
}

/* Подменяем оконную процедуру самого текстового поля - только затем,
 * чтобы поймать Ctrl+A (выделить всё содержимое целиком, например,
 * чтобы потом скопировать Ctrl+C - поле только для чтения, но
 * копирование из него разрешено). Обычный EDIT/RichEdit не делает
 * этого сам по себе без явной обработки. */
static LRESULT CALLBACK ViewerEditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == 'F' && (GetKeyState(VK_CONTROL) & 0x8000) && g_hEditFind) {
        SetFocus(g_hEditFind); /* Ctrl+F - в поле поиска этого окна */
        SendMessageW(g_hEditFind, EM_SETSEL, 0, -1);
        return 0;
    }
    if (msg == WM_KEYDOWN && wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        clear_found_highlight(); /* иначе подсветка найденного слова будет видна поверх выделения */
        SendMessageW(hwnd, EM_SETSEL, 0, -1);
        return 0;
    }
    return CallWindowProcW(g_origViewerEditProc, hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK TextViewerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (g_hEditFind)
            MoveWindow(g_hEditFind, 8, 8, 300, 24, TRUE);
        if (g_hBtnFindNext)
            MoveWindow(g_hBtnFindNext, 316, 8, 130, 24, TRUE);
        if (g_hLabelFindCount)
            MoveWindow(g_hLabelFindCount, 454, 12, 160, 20, TRUE);
        if (g_hEditTextViewer)
            MoveWindow(g_hEditTextViewer, 8, 40, rc.right - 16, rc.bottom - 48, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_VIEWER_BTN_FINDNEXT) {
            find_next_in_viewer();
            return 0;
        }
        if (LOWORD(wParam) == IDC_VIEWER_EDIT_FIND && HIWORD(wParam) == EN_CHANGE) {
            find_next_in_viewer_ex(1); /* поиск по ходу набора: окно сразу показывает первое вхождение */
            return 0;
        }
        break;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, CLR_FG_LABEL);
        SetBkColor(hdc, CLR_BG_LOG);
        SetBkMode(hdc, OPAQUE);
        return (LRESULT)g_hbrLogBg; /* кисть должна совпадать с SetBkColor выше - иначе
                                      * область вокруг символов текста будет другого
                                      * оттенка, чем всё остальное поле */
    }
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* Создаёт (при первом вызове) или переиспользует окно-просмотрщик,
 * заполняет его заголовком и текстом, показывает поверх остальных.
 * text может быть сколь угодно длинным - ограничение снято явно
 * (EM_EXLIMITTEXT), в отличие от MessageBoxW. */
static void show_text_window(HWND parent, const WCHAR *title, const WCHAR *text) {
    if (!g_hwndTextViewer) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = TextViewerWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"AcarsTextViewerWndClass";
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = g_hbrMainBg;
        wc.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APPICON));
        RegisterClassW(&wc); /* если уже зарегистрирован (маловероятно) - просто вернёт 0, не страшно */

        g_hwndTextViewer = CreateWindowW(L"AcarsTextViewerWndClass", title,
                                          WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                          820, 620, parent, NULL, GetModuleHandleW(NULL), NULL);
        if (!g_hwndTextViewer)
            return; /* не смогли создать окно - просто ничего не покажем, не падаем */

        HMODULE hRichEdit = LoadLibraryW(L"riched20.dll");
        LPCWSTR editClass = hRichEdit ? L"RichEdit20W" : L"EDIT";
        g_viewerIsRichEdit = hRichEdit ? 1 : 0;
        g_hEditTextViewer = CreateWindowW(editClass, NULL,
                                           WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER |
                                               ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                           8, 40, 800, 568, g_hwndTextViewer, NULL, NULL, NULL);
        if (g_hEditTextViewer) {
            /* Подклассирование - см. ViewerEditSubclassProc выше -
             * только чтобы поймать Ctrl+A (выделить всё). */
            g_origViewerEditProc = (WNDPROC)SetWindowLongPtrW(
                g_hEditTextViewer, GWLP_WNDPROC, (LONG_PTR)ViewerEditSubclassProc);
        }

        g_hEditFind = CreateWindowW(L"EDIT", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER,
                                     8, 8, 300, 24, g_hwndTextViewer,
                                     (HMENU)IDC_VIEWER_EDIT_FIND, NULL, NULL);
        g_hBtnFindNext = CreateWindowW(L"BUTTON", L"Найти далее",
                                        WS_CHILD | WS_VISIBLE | BS_FLAT, 316, 8, 130, 24,
                                        g_hwndTextViewer, (HMENU)IDC_VIEWER_BTN_FINDNEXT, NULL,
                                        NULL);
        g_hLabelFindCount = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                           454, 12, 160, 20, g_hwndTextViewer, NULL, NULL, NULL);
        if (g_hEditFind) {
            SendMessageW(g_hEditFind, EM_SETCUEBANNER, TRUE,
                         (LPARAM)L"Найти в этом окне (Enter - дальше, Ctrl+F)");
            /* Подклассирование - см. комментарий у FindEditSubclassProc
             * выше - только чтобы поймать нажатие Enter прямо в поле. */
            g_origFindEditProc =
                (WNDPROC)SetWindowLongPtrW(g_hEditFind, GWLP_WNDPROC, (LONG_PTR)FindEditSubclassProc);
        }

        if (g_hEditTextViewer) {
            HFONT hfont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       RUSSIAN_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
            SendMessageW(g_hEditTextViewer, WM_SETFONT, (WPARAM)hfont, TRUE);
            if (g_hEditFind)
                SendMessageW(g_hEditFind, WM_SETFONT, (WPARAM)hfont, TRUE);
            if (g_hBtnFindNext)
                SendMessageW(g_hBtnFindNext, WM_SETFONT, (WPARAM)hfont, TRUE);
            if (g_hLabelFindCount)
                SendMessageW(g_hLabelFindCount, WM_SETFONT, (WPARAM)hfont, TRUE);
            if (hRichEdit) {
                SendMessageW(g_hEditTextViewer, EM_SETBKGNDCOLOR, 0, (LPARAM)CLR_BG_LOG);
                SendMessageW(g_hEditTextViewer, EM_EXLIMITTEXT, 0, (LPARAM)16000000);
            } else {
                SendMessageW(g_hEditTextViewer, EM_SETLIMITTEXT, 16000000, 0);
            }
        }
    }

    SetWindowTextW(g_hwndTextViewer, title);
    if (g_hEditTextViewer) {
        g_lastFoundPos = -1; /* новый текст - прежняя позиция подсветки больше не имеет смысла */
        g_lastFoundLen = 0;
        g_hlAllActive = 0;
        g_lastSearchText[0] = L'\0'; /* и прежний запрос в этом новом тексте ещё не искался */
        g_matchIndex = 0;
        g_matchTotal = 0;
        if (g_hLabelFindCount)
            SetWindowTextW(g_hLabelFindCount, L"");
        SetWindowTextW(g_hEditTextViewer, text);

        /* ВАЖНО: WM_CTLCOLOREDIT (см. TextViewerWndProc) красит текст
         * только у ОБЫЧНОГО EDIT - у RichEdit это сообщение ни на что
         * не влияет, а цвет текста по умолчанию у RichEdit - чёрный.
         * Чёрный текст на нашем тёмном фоне (EM_SETBKGNDCOLOR выше)
         * был попросту невидим. Выделяем весь текст и красим его явно
         * через EM_SETCHARFORMAT - для RichEdit это и есть способ
         * задать цвет текста; для обычного EDIT (если RichEdit не
         * загрузился) это сообщение просто ничего не делает, вреда
         * нет, а цвет там уже и так обеспечивает WM_CTLCOLOREDIT. */
        SendMessageW(g_hEditTextViewer, EM_SETSEL, 0, -1);
        CHARFORMAT2W cf;
        memset(&cf, 0, sizeof(cf));
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR;
        cf.crTextColor = CLR_FG_LABEL;
        SendMessageW(g_hEditTextViewer, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
        SendMessageW(g_hEditTextViewer, EM_SETSEL, 0, 0); /* снимаем выделение, курсор в начало */
    }
    ShowWindow(g_hwndTextViewer, SW_SHOW);
    SetForegroundWindow(g_hwndTextViewer);
}

/* Единое окно "Подробности" - по каждому сообщению, в котором нашлось
 * хоть что-то из маршрута/телеметрии/погоды, показывает ИСХОДНЫЙ текст
 * и построчную расшифровку "какой кусок текста что означает"
 * (ooi_format_annotated/asu_telemetry_format_annotated/
 * metar_format_annotated - все три используют общий принцип: не
 * просто готовое значение, а явная связь с конкретным фрагментом
 * исходного текста). Показывает последние MAX_DETAILS_MESSAGES
 * подходящих сообщений - при большом количестве полный список проще
 * смотреть в CSV/JSON логе (см. README). */
#define MAX_DETAILS_MESSAGES 12

static void show_details(HWND hwnd) {
    if (g_all_rows_count == 0) {
        MessageBoxW(hwnd, L"Пока не принято ни одного сообщения.", L"Подробности",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    /* Собираем индексы ПОДХОДЯЩИХ строк (есть маршрут, телеметрия или
     * погода), с конца (самые свежие) - g_all_rows идёт в
     * хронологическом порядке получения. */
    int *matched = (int *)malloc(sizeof(int) * (size_t)g_all_rows_count);
    int n_matched = 0;
    if (!matched) {
        MessageBoxW(hwnd, L"Не хватило памяти.", L"Подробности", MB_OK | MB_ICONWARNING);
        return;
    }

    for (int i = g_all_rows_count - 1; i >= 0 && n_matched < MAX_DETAILS_MESSAGES; i--) {
        TableRow *row = &g_all_rows[i];
        if (!row->text[0])
            continue;
        char text_utf8[1200];
        wide_to_utf8(row->text, text_utf8, sizeof(text_utf8));

        OoiInfo ooi;
        AsuTelemetry tel;
        MetarReport mr[4];
        int has_route = ooi_parse(text_utf8, &ooi);
        int has_tel = asu_telemetry_parse(text_utf8, &tel);
        int has_weather = metar_find_reports(text_utf8, mr, 4) > 0;
        int has_report = 0;
        {
            char lab[8], rbuf[3072];
            wide_to_utf8(row->label, lab, sizeof(lab));
            /* служебные короткие метки (проверка канала и т.п.) в
             * "Подробности" не включаем - они вытеснили бы содержательные */
            if (strcmp(lab, "Q0") && strcmp(lab, "5P") && strcmp(lab, "Q6") &&
                strcmp(lab, "5V") && strcmp(lab, "SA") && strcmp(lab, "SQ"))
                has_report = acars_report_decode(lab, text_utf8, rbuf, sizeof(rbuf));
        }

        if (has_route || has_tel || has_weather || has_report)
            matched[n_matched++] = i;
    }

    if (n_matched == 0) {
        MessageBoxW(hwnd,
                     L"В принятых сообщениях не найдено ни маршрута, ни телеметрии,\r\n"
                     L"ни погоды. Это нормально, если таких сообщений просто не было.",
                     L"Подробности", MB_OK | MB_ICONINFORMATION);
        free(matched);
        return;
    }

    WCHAR *buf = (WCHAR *)malloc(sizeof(WCHAR) * (size_t)(4096 + n_matched * 6000));
    if (!buf) {
        free(matched);
        MessageBoxW(hwnd, L"Не хватило памяти.", L"Подробности", MB_OK | MB_ICONWARNING);
        return;
    }
    int pos = wsprintfW(buf, L"Подробный разбор (последние %d сообщений с маршрутом/"
                              L"телеметрией/погодой):\r\n",
                         n_matched);

    for (int k = 0; k < n_matched; k++) {
        TableRow *row = &g_all_rows[matched[k]];
        char text_utf8[1200];
        wide_to_utf8(row->text, text_utf8, sizeof(text_utf8));

        pos += wsprintfW(buf + pos,
                          L"\r\n==============================================\r\n"
                          L"Борт:  %s      Время:  %s      Метка:  %s\r\n",
                          row->addr, row->time, row->label);
        pos += wsprintfW(buf + pos, L"Исходный текст:\r\n%s\r\n", row->text);

        char abuf[1536];

        {
            char lab[8], rbuf[3072];
            wide_to_utf8(row->label, lab, sizeof(lab));
            if (acars_report_decode(lab, text_utf8, rbuf, sizeof(rbuf))) {
                WCHAR *w = utf8_to_wide(rbuf);
                if (w) {
                    pos += wsprintfW(buf + pos, L"Расшифровка:\r\n");
                    /* wsprintfW ограничен 1024 символами - длинный текст
                     * копируем вручную, заменяя \n на \r\n */
                    for (const WCHAR *q = w; *q && pos < 4096 + n_matched * 6000 - 8; q++) {
                        if (*q == L'\n') buf[pos++] = L'\r';
                        buf[pos++] = *q;
                    }
                    buf[pos++] = L'\r'; buf[pos++] = L'\n';
                    buf[pos] = 0;
                    free(w);
                }
            }
        }

        ooi_format_annotated(text_utf8, abuf, sizeof(abuf));
        if (abuf[0]) {
            WCHAR *w = utf8_to_wide(abuf);
            if (w) {
                pos += wsprintfW(buf + pos, L"Разбор (маршрут, формат OOOI FI):\r\n%s", w);
                free(w);
            }
        }

        asu_telemetry_format_annotated(text_utf8, abuf, sizeof(abuf));
        if (abuf[0]) {
            WCHAR *w = utf8_to_wide(abuf);
            if (w) {
                pos += wsprintfW(buf + pos, L"Разбор (телеметрия ASU/AFL):\r\n%s", w);
                free(w);
            }
        }

        MetarReport mr[4];
        int n_mr = metar_find_reports(text_utf8, mr, 4);
        for (int mi = 0; mi < n_mr; mi++) {
            const char *rem = text_utf8;
            /* ищем в исходном тексте начало именно этой сводки по коду
             * станции, чтобы аннотировать только её часть */
            const char *st = strstr(rem, mr[mi].station);
            metar_format_annotated(st ? st : text_utf8, abuf, sizeof(abuf));
            if (abuf[0]) {
                WCHAR *w = utf8_to_wide(abuf);
                WCHAR *station_w = utf8_to_wide(mr[mi].station);
                if (w) {
                    pos += wsprintfW(buf + pos, L"Разбор (погода METAR, %s):\r\n%s",
                                      station_w ? station_w : L"?", w);
                    free(w);
                }
                free(station_w);
            }
        }

        if (pos > (int)(4096 + (size_t)n_matched * 6000) - 400)
            break; /* защита от переполнения буфера на неожиданно длинных данных */
    }

    show_text_window(hwnd, L"Подробности", buf);
    free(buf);
    free(matched);
}

/* Показывает все METAR-сводки погоды (см. metar.c - настоящий
 * международный стандарт ICAO Annex 3, не фирменный формат), найденные
 * в тексте уже накопленных сообщений за сессию - по станциям, без
 * повторов одной и той же сводки (одинаковый код станции + время). */
#define MAX_WEATHER_REPORTS 100

typedef struct {
    MetarReport report;
    WCHAR addr[16];
    WCHAR received_time[32]; /* время приёма ACARS-сообщения (не время самой сводки METAR -
                               * оно, если есть, видно внутри самой сводки) */
} WeatherEntry;

static void show_weather(HWND hwnd) {
    if (g_all_rows_count == 0) {
        MessageBoxW(hwnd, L"Пока не принято ни одного сообщения.", L"Погода",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    WeatherEntry *reports = (WeatherEntry *)calloc(MAX_WEATHER_REPORTS, sizeof(WeatherEntry));
    if (!reports) {
        MessageBoxW(hwnd, L"Не хватило памяти.", L"Погода", MB_OK | MB_ICONWARNING);
        return;
    }
    int n_reports = 0;

    for (int i = 0; i < g_all_rows_count && n_reports < MAX_WEATHER_REPORTS; i++) {
        TableRow *row = &g_all_rows[i];
        if (!row->text[0])
            continue;

        char text_utf8[1200];
        wide_to_utf8(row->text, text_utf8, sizeof(text_utf8));

        MetarReport found[4];
        int n_found = metar_find_reports(text_utf8, found, 4);
        for (int f = 0; f < n_found && n_reports < MAX_WEATHER_REPORTS; f++) {
            int dup = 0;
            for (int j = 0; j < n_reports; j++) {
                if (strcmp(reports[j].report.station, found[f].station) == 0 &&
                    reports[j].report.day == found[f].day &&
                    reports[j].report.hour == found[f].hour &&
                    reports[j].report.minute == found[f].minute) {
                    dup = 1;
                    break;
                }
            }
            if (!dup) {
                reports[n_reports].report = found[f];
                reg_key(row->addr, reports[n_reports].addr);
                reports[n_reports].addr[15] = L'\0';
                wcsncpy(reports[n_reports].received_time, row->time, 31);
                reports[n_reports].received_time[31] = L'\0';
                n_reports++;
            }
        }
    }

    if (n_reports == 0) {
        MessageBoxW(hwnd,
                     L"В принятых сообщениях не найдено сводок погоды METAR.\r\n"
                     L"Это нормально, если таких сообщений просто не было.",
                     L"Погода", MB_OK | MB_ICONINFORMATION);
        free(reports);
        return;
    }

    WCHAR *buf = (WCHAR *)malloc(sizeof(WCHAR) * (size_t)(4096 + n_reports * 650));
    if (!buf) {
        free(reports);
        MessageBoxW(hwnd, L"Не хватило памяти.", L"Погода", MB_OK | MB_ICONWARNING);
        return;
    }
    int pos = wsprintfW(buf, L"Найденные сводки погоды за сессию (%d):\r\n\r\n", n_reports);

    for (int i = 0; i < n_reports; i++) {
        pos += wsprintfW(buf + pos, L"Принято от борта: %s      Время приёма: %s\r\n",
                          reports[i].addr, reports[i].received_time);
        char mbuf[1024];
        metar_format(&reports[i].report, mbuf, sizeof(mbuf));
        WCHAR *w = utf8_to_wide(mbuf);
        if (w) {
            pos += wsprintfW(buf + pos, L"%s\r\n", w);
            free(w);
        }
    }

    show_text_window(hwnd, L"Погода", buf);
    free(buf);
    free(reports);
}

/* Поставщик данных для встроенной карты (см. map_view.c): собирает из
 * накопленных сообщений (g_all_rows - и принятых вживую, и загруженных
 * из WAV, и импортированных из журнала) всё, что известно о каждом
 * борте с координатами в телеметрии ASU/AFL (поля LATN/LONE): последнюю
 * позицию, весь след по порядку получения и сопутствующие значения.
 * Окно карты вызывает эту функцию само - при открытии и раз в пару
 * секунд, поэтому новые позиции появляются на карте без нажатий. */
/* День месяца из "группы времени" служебного отчёта ("/03031424..." -> 3) или -1, если такой группы нет. */
static int msg_group_day(const char *text) {
    const char *sl = strchr(text, '/');
    if (!sl || sl - text > 40)
        return -1;
    if (!isdigit((unsigned char)sl[1]) || !isdigit((unsigned char)sl[2]))
        return -1;
    int d = (sl[1] - '0') * 10 + (sl[2] - '0');
    return (d >= 1 && d <= 31) ? d : -1;
}

static long row_epoch(const TableRow *row) {
    char tb[40];
    wide_to_utf8(row->time, tb, sizeof tb);
    return geo_time_parse(tb);
}

static int map_provider(MapAircraft *out, int max_out, long cutoff) {
    int n = 0;
    for (int i = 0; i < g_all_rows_count; i++) {
        TableRow *row = &g_all_rows[i];
        if (!row->text[0])
            continue;
        if (cutoff > 0 && row_epoch(row) > cutoff)
            continue; /* проигрывание истории: что было позже выбранного момента - ещё не случилось */
        char text_utf8[1200];
        wide_to_utf8(row->text, text_utf8, sizeof(text_utf8));

        AsuTelemetry tel;
        if (!asu_telemetry_parse(text_utf8, &tel) || !tel.lat[0] || !tel.lon[0])
            continue;
        double lat, lon;
        if (!geo_parse_coord(tel.lat, &lat) || !geo_parse_coord(tel.lon, &lon))
            continue;
        if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0)
            continue; /* искажённое сообщение - координаты вне земного шара, не рисуем */

        /* отложенное сообщение: борт досылает накопленные отчёты, и в их заголовке стоит число месяца события
         * (у RA89042 на 6-е число пришли события 3-го). Такая позиция - не текущее положение борта,
         * на карту её не наносим (в отчёте по борту и в таблицах она остаётся). */
        {
            int gd = msg_group_day(text_utf8);
            int Y2, Mo2, D2;
            char tb[40];
            wide_to_utf8(row->time, tb, sizeof tb);
            if (gd > 0 && sscanf(tb, "%d-%d-%d", &Y2, &Mo2, &D2) == 3) {
                int diff = (D2 - gd + 31) % 31;
                if (diff >= 2 && diff <= 20)
                    continue;
            }
        }

        WCHAR key[16];
        reg_key(row->addr, key);
        char reg_full[40];
        wide_to_utf8(key, reg_full, sizeof(reg_full));
        const char *reg = reg_full[0] == '.' ? reg_full + 1 : reg_full;
        if (!reg[0])
            continue;

        int idx = -1;
        for (int j = 0; j < n; j++) {
            if (strcmp(out[j].reg, reg) == 0) {
                idx = j;
                break;
            }
        }
        if (idx < 0) {
            if (n >= max_out)
                continue;
            idx = n++;
            memset(&out[idx], 0, sizeof(out[idx]));
            safe_copy_local(out[idx].reg, sizeof(out[idx].reg), reg);
            AircraftInfo ac;
            if (aircraft_db_lookup(reg_full, &ac)) {
                snprintf(out[idx].info, sizeof(out[idx].info), "%.100s %.100s%s%.20s%s%s%.80s",
                         ac.manufacturer, ac.model, ac.typecode[0] ? " (" : "", ac.typecode,
                         ac.typecode[0] ? ")" : "", ac.operator_name[0] ? ", " : "",
                         ac.operator_name);
                /* код Mode S нужен окну карты, чтобы запрашивать фото именно этого борта */
                safe_copy_local(out[idx].hex, sizeof(out[idx].hex), ac.icao24);
                safe_copy_local(out[idx].type, sizeof(out[idx].type), ac.typecode);
                safe_copy_local(out[idx].model, sizeof(out[idx].model), ac.model);
                safe_copy_local(out[idx].operator_name, sizeof(out[idx].operator_name), ac.operator_name);
                for (char *h = out[idx].hex; *h; h++)
                    if (*h >= 'a' && *h <= 'z')
                        *h = (char)(*h - 'a' + 'A');
            }
        }

        MapAircraft *a = &out[idx];
        a->lat = lat;
        a->lon = lon;
        wide_to_utf8(row->time, a->time, sizeof(a->time));
        if (tel.alt[0]) safe_copy_local(a->alt, sizeof(a->alt), tel.alt);
        if (tel.fuel[0]) safe_copy_local(a->fuel, sizeof(a->fuel), tel.fuel);
        if (tel.eta[0]) safe_copy_local(a->eta, sizeof(a->eta), tel.eta);

        /* след: добавляем позицию, только если она реально сдвинулась
         * (иначе повтор того же сообщения забьёт след одинаковыми точками) */
        /* порядковый номер сообщения борта (MSN) и время приёма: сообщения иногда доходят не по порядку
         * (борт досылает накопленное), и тогда по времени приёма след рисует "треугольник". */
        int seq = -1;
        {
            AcarsHeader hd;
            if (acars_header_parse(text_utf8, &hd) && hd.valid)
                seq = hd.seq;
        }
        long recv = row_epoch(row);
        /* новый рейс (или перерыв больше 4 часов) - прошлый след не продолжаем: иначе линия соединяет
         * посадку одного рейса со взлётом другого (так выглядело у RA73170: прилёт из Тюмени и вылет в Актау) */
        {
            char lab2[8];
            wide_to_utf8(row->label, lab2, sizeof lab2);
            AcarsSummary sm2;
            const char *fl = (acars_report_summary(lab2, text_utf8, &sm2) && sm2.flight[0]) ? sm2.flight : "";
            int newflight = fl[0] && a->trail_flight[0] && strcmp(fl, a->trail_flight) != 0;
            int gap = a->n_trail > 0 && recv > 0 && a->trail_recv[a->n_trail - 1] > 0 &&
                      recv - a->trail_recv[a->n_trail - 1] > 4 * 3600L;
            if (newflight || gap)
                a->n_trail = 0;
            if (fl[0])
                safe_copy_local(a->trail_flight, sizeof(a->trail_flight), fl);
        }
        /* повтор уже имеющейся точки (то же сообщение приняли дважды) - пропускаем */
        int dup = 0;
        for (int k = a->n_trail - 1; k >= 0 && k >= a->n_trail - 6; k--)
            if (fabs(a->trail_lat[k] - lat) <= 1e-5 && fabs(a->trail_lon[k] - lon) <= 1e-5 &&
                a->trail_seq[k] == seq) {
                dup = 1;
                break;
            }
        int moved = !dup && (a->n_trail == 0 ||
                    fabs(a->trail_lat[a->n_trail - 1] - lat) > 1e-5 ||
                    fabs(a->trail_lon[a->n_trail - 1] - lon) > 1e-5);
        if (moved) {
            if (a->n_trail >= MAP_MAX_TRAIL) {
                size_t k = MAP_MAX_TRAIL - 1;
                memmove(&a->trail_lat[0], &a->trail_lat[1], k * sizeof(double));
                memmove(&a->trail_lon[0], &a->trail_lon[1], k * sizeof(double));
                memmove(&a->trail_time[0], &a->trail_time[1], k * sizeof(a->trail_time[0]));
                memmove(&a->trail_label[0], &a->trail_label[1], k * sizeof(a->trail_label[0]));
                memmove(&a->trail_text[0], &a->trail_text[1], k * sizeof(a->trail_text[0]));
                memmove(&a->trail_alt[0], &a->trail_alt[1], k * sizeof(a->trail_alt[0]));
                memmove(&a->trail_fuel[0], &a->trail_fuel[1], k * sizeof(a->trail_fuel[0]));
                memmove(&a->trail_eta[0], &a->trail_eta[1], k * sizeof(a->trail_eta[0]));
                memmove(&a->trail_seq[0], &a->trail_seq[1], k * sizeof(int));
                memmove(&a->trail_recv[0], &a->trail_recv[1], k * sizeof(long));
                a->n_trail = MAP_MAX_TRAIL - 1;
            }
            int t = a->n_trail;
            a->trail_lat[t] = lat;
            a->trail_lon[t] = lon;
            a->trail_seq[t] = seq;
            a->trail_recv[t] = recv;
            wide_to_utf8(row->time, a->trail_time[t], sizeof(a->trail_time[t]));
            wide_to_utf8(row->label, a->trail_label[t], sizeof(a->trail_label[t]));
            safe_copy_local(a->trail_text[t], sizeof(a->trail_text[t]), text_utf8);
            safe_copy_local(a->trail_alt[t], sizeof(a->trail_alt[t]), tel.alt);
            safe_copy_local(a->trail_fuel[t], sizeof(a->trail_fuel[t]), tel.fuel);
            safe_copy_local(a->trail_eta[t], sizeof(a->trail_eta[t]), tel.eta);
            a->n_trail++;
            /* пришло раньше более старого сообщения (меньший номер, принято в пределах 5 минут) -
             * ставим точку на своё место по номеру */
            while (t > 0 && seq >= 0 && a->trail_seq[t - 1] >= 0 && a->trail_seq[t - 1] > seq &&
                   a->trail_seq[t - 1] - seq < 50 && recv - a->trail_recv[t - 1] < 300) {
                int u = t - 1;
                double dtmp;
                char c32[32], c4[4], c400[400], c16[16];
                int itmp; long ltmp;
#define SWP(arr, tmp, sz) do { memcpy(tmp, &(arr)[u], sz); memcpy(&(arr)[u], &(arr)[t], sz); memcpy(&(arr)[t], tmp, sz); } while (0)
                SWP(a->trail_lat, &dtmp, sizeof dtmp);
                SWP(a->trail_lon, &dtmp, sizeof dtmp);
                SWP(a->trail_time, c32, sizeof c32);
                SWP(a->trail_label, c4, sizeof c4);
                SWP(a->trail_text, c400, sizeof c400);
                SWP(a->trail_alt, c16, sizeof c16);
                SWP(a->trail_fuel, c16, sizeof c16);
                SWP(a->trail_eta, c16, sizeof c16);
                SWP(a->trail_seq, &itmp, sizeof itmp);
                SWP(a->trail_recv, &ltmp, sizeof ltmp);
#undef SWP
                t = u;
            }
            /* "последняя позиция" борта - конец упорядоченного следа, а не последнее принятое сообщение */
            int e = a->n_trail - 1;
            a->lat = a->trail_lat[e];
            a->lon = a->trail_lon[e];
            safe_copy_local(a->time, sizeof(a->time), a->trail_time[e]);
            if (a->trail_alt[e][0]) safe_copy_local(a->alt, sizeof(a->alt), a->trail_alt[e]);
            if (a->trail_fuel[e][0]) safe_copy_local(a->fuel, sizeof(a->fuel), a->trail_fuel[e]);
            if (a->trail_eta[e][0]) safe_copy_local(a->eta, sizeof(a->eta), a->trail_eta[e]);
        }
    }

    /* маршрут и рейс: берём из ЛЮБЫХ сообщений борта (в отчётах без координат тоже есть
     * аэропорты вылета/назначения), последнее известное значение; вылет и назначение - парой */
    for (int i = 0; i < g_all_rows_count && n > 0; i++) {
        TableRow *row = &g_all_rows[i];
        if (!row->text[0])
            continue;
        if (cutoff > 0 && row_epoch(row) > cutoff)
            continue;
        WCHAR key[16];
        reg_key(row->addr, key);
        char reg_full[40];
        wide_to_utf8(key, reg_full, sizeof(reg_full));
        const char *reg = reg_full[0] == '.' ? reg_full + 1 : reg_full;
        int idx = -1;
        for (int j = 0; j < n; j++)
            if (strcmp(out[j].reg, reg) == 0) { idx = j; break; }
        if (idx < 0)
            continue;
        char text_utf8[1200], lab[8];
        wide_to_utf8(row->text, text_utf8, sizeof(text_utf8));
        wide_to_utf8(row->label, lab, sizeof(lab));
        AcarsSummary sm;
        if (!acars_report_summary(lab, text_utf8, &sm))
            continue;
        if (sm.flight[0])
            safe_copy_local(out[idx].flight, sizeof(out[idx].flight), sm.flight);
        if (sm.dep[0] && sm.dest[0]) {
            safe_copy_local(out[idx].dep, sizeof(out[idx].dep), sm.dep);
            safe_copy_local(out[idx].dest, sizeof(out[idx].dest), sm.dest);
        }
    }
    /* в прошлом: борт, от которого последнее сообщение было больше 2 часов назад, уже не в воздухе - не показываем */
    if (cutoff > 0) {
        int w = 0;
        for (int j = 0; j < n; j++) {
            int last = out[j].n_trail - 1;
            if (last >= 0 && cutoff - out[j].trail_recv[last] <= 2 * 3600L) {
                if (w != j)
                    out[w] = out[j];
                w++;
            }
        }
        n = w;
    }
    return n;
}

/* Границы времени сообщений с координатами - для ползунка проигрывания истории на карте. */
static void map_time_range(long *t_min, long *t_max) {
    long lo = 0, hi = 0;
    for (int i = 0; i < g_all_rows_count; i++) {
        const TableRow *row = &g_all_rows[i];
        if (!row->text[0] || !wcsstr(row->text, L"LAT"))
            continue;
        long t = row_epoch(row);
        if (t <= 0)
            continue;
        if (!lo || t < lo) lo = t;
        if (t > hi) hi = t;
    }
    *t_min = lo;
    *t_max = hi;
}

/* "Все сообщения борта": спрашивает период (пустые поля - без ограничения), собирает сообщения борта
 * из g_all_rows (журналы за все загруженные дни + принятое сейчас) и показывает отчёт. */
static void aircraft_report_dialog(HWND owner, const char *reg);
static void map_report_cb(HWND owner, const char *reg) { aircraft_report_dialog(owner, reg); }

/* Кнопка "Карта" - открывает встроенную карту с самолётами (map_view.c):
 * подложка OpenStreetMap, значок самолёта на позиции каждого борта,
 * поворот по курсу, след, список бортов и подробности справа. */
static void show_map(HWND hwnd) {
    mapview_set_report_cb(map_report_cb);
    mapview_set_range_cb(map_time_range);
    mapview_show(hwnd, map_provider);
}

/* Показывает телеметрию (топливо/температура/ветер/координаты/ETA/
 * высота, см. asu_telemetry.c) по каждому борту - ПОСЛЕДНЕЕ известное
 * значение каждого поля за сессию (не список всех замеров подряд -
 * g_all_rows идёт в хронологическом порядке получения, поэтому
 * перезапись при повторном обнаружении того же борта естественно даёт
 * самые свежие данные). */
typedef struct {
    WCHAR addr[16];
    WCHAR time[32]; /* время ПОСЛЕДНЕГО (самого свежего) сообщения, из которого взяты эти данные */
    char fuel[16], temp[16], wind_dir[16], wind_speed[16];
    char lat[24], lon[24], eta[16], alt[16];
} TelemetryEntry;

#define MAX_TELEMETRY_ENTRIES 200

static void show_telemetry(HWND hwnd) {
    if (g_all_rows_count == 0) {
        MessageBoxW(hwnd, L"Пока не принято ни одного сообщения.", L"Телеметрия",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    TelemetryEntry *entries = (TelemetryEntry *)calloc(MAX_TELEMETRY_ENTRIES, sizeof(TelemetryEntry));
    if (!entries) {
        MessageBoxW(hwnd, L"Не хватило памяти.", L"Телеметрия", MB_OK | MB_ICONWARNING);
        return;
    }
    int n_entries = 0;

    for (int i = 0; i < g_all_rows_count; i++) {
        TableRow *row = &g_all_rows[i];
        if (!row->text[0])
            continue;

        char text_utf8[1200];
        wide_to_utf8(row->text, text_utf8, sizeof(text_utf8));

        AsuTelemetry tel;
        if (!asu_telemetry_parse(text_utf8, &tel))
            continue;

        int idx = -1;
        for (int j = 0; j < n_entries; j++) {
            if (reg_matches(entries[j].addr, row->addr)) {
                idx = j;
                break;
            }
        }
        if (idx < 0) {
            if (n_entries >= MAX_TELEMETRY_ENTRIES)
                continue;
            idx = n_entries++;
            reg_key(row->addr, entries[idx].addr);
            entries[idx].addr[15] = L'\0';
        }

        TelemetryEntry *e = &entries[idx];
        /* время берём от КАЖДОГО сообщения, из которого реально взяли
         * хоть одно значение - так "время" всегда соответствует тому,
         * что сейчас показано, даже если какие-то поля обновлялись в
         * разные моменты (на практике все поля обычно приходят вместе,
         * в одном сообщении). */
        int got_any = tel.fuel[0] || tel.temp[0] || tel.wind_dir[0] || tel.wind_speed[0] ||
                      tel.lat[0] || tel.lon[0] || tel.eta[0] || tel.alt[0];
        if (got_any) {
            wcsncpy(e->time, row->time, 31);
            e->time[31] = L'\0';
        }
        if (tel.fuel[0])
            safe_copy_local(e->fuel, sizeof(e->fuel), tel.fuel);
        if (tel.temp[0])
            safe_copy_local(e->temp, sizeof(e->temp), tel.temp);
        if (tel.wind_dir[0])
            safe_copy_local(e->wind_dir, sizeof(e->wind_dir), tel.wind_dir);
        if (tel.wind_speed[0])
            safe_copy_local(e->wind_speed, sizeof(e->wind_speed), tel.wind_speed);
        if (tel.lat[0])
            safe_copy_local(e->lat, sizeof(e->lat), tel.lat);
        if (tel.lon[0])
            safe_copy_local(e->lon, sizeof(e->lon), tel.lon);
        if (tel.eta[0])
            safe_copy_local(e->eta, sizeof(e->eta), tel.eta);
        if (tel.alt[0])
            safe_copy_local(e->alt, sizeof(e->alt), tel.alt);
    }

    if (n_entries == 0) {
        MessageBoxW(hwnd,
                     L"В принятых сообщениях не найдено телеметрии ASU/AFL\r\n"
                     L"(метки 26/27 - топливо, координаты, погода). Это нормально,\r\n"
                     L"если таких сообщений просто не было - см. README.",
                     L"Телеметрия", MB_OK | MB_ICONINFORMATION);
        free(entries);
        return;
    }

    WCHAR *buf = (WCHAR *)malloc(sizeof(WCHAR) * (size_t)(4096 + n_entries * 300));
    if (!buf) {
        free(entries);
        MessageBoxW(hwnd, L"Не хватило памяти.", L"Телеметрия", MB_OK | MB_ICONWARNING);
        return;
    }
    int pos = wsprintfW(buf, L"Последняя известная телеметрия за сессию (%d бортов):\r\n\r\n",
                         n_entries);

    for (int i = 0; i < n_entries; i++) {
        TelemetryEntry *e = &entries[i];
        pos += wsprintfW(buf + pos, L"Борт: %s      Время: %s\r\n", e->addr,
                          e->time[0] ? e->time : L"?");
        if (e->fuel[0]) {
            WCHAR *w = utf8_to_wide(e->fuel);
            pos += wsprintfW(buf + pos, L"  Топливо    : %s\r\n", w ? w : L"?");
            free(w);
        }
        if (e->temp[0]) {
            WCHAR *w = utf8_to_wide(e->temp);
            pos += wsprintfW(buf + pos, L"  Температура: %s\r\n", w ? w : L"?");
            free(w);
        }
        if (e->wind_dir[0] || e->wind_speed[0]) {
            char wt[200];
            wind_text(e->wind_dir, e->wind_speed, wt, sizeof wt);
            WCHAR *wd = utf8_to_wide(wt);
            pos += wsprintfW(buf + pos, L"  Ветер      : %s\r\n", wd ? wd : L"?");
            free(wd);
        }
        if (e->lat[0] || e->lon[0]) {
            WCHAR *la = utf8_to_wide(e->lat[0] ? e->lat : "?");
            WCHAR *lo = utf8_to_wide(e->lon[0] ? e->lon : "?");
            pos += wsprintfW(buf + pos, L"  Координаты : %s, %s\r\n", la ? la : L"?", lo ? lo : L"?");
            free(la);
            free(lo);
        }
        if (e->eta[0]) {
            WCHAR *w = utf8_to_wide(e->eta);
            pos += wsprintfW(buf + pos, L"  ETA        : %s\r\n", w ? w : L"?");
            free(w);
        }
        if (e->alt[0]) {
            WCHAR *w = utf8_to_wide(e->alt);
            pos += wsprintfW(buf + pos, L"  Высота     : %s\r\n", w ? w : L"?");
            free(w);
        }
        pos += wsprintfW(buf + pos, L"\r\n");
    }

    show_text_window(hwnd, L"Телеметрия", buf);
    free(buf);
    free(entries);
}

/* Показывает отчёты о неисправностях/предупреждениях (CFDIU, см.
 * cfdiu_fault.c), найденные в тексте уже накопленных сообщений за
 * сессию - по каждому борту, без дублей одинаковых описаний. Тот же
 * простой формат "разовый снимок по запросу". */
typedef struct {
    WCHAR addr[16];
    WCHAR time[32];
    char severity[8];
    char description[160];
} FaultEntry;

#define MAX_FAULT_ENTRIES 200

static void show_faults(HWND hwnd) {
    if (g_all_rows_count == 0) {
        MessageBoxW(hwnd, L"Пока не принято ни одного сообщения.", L"Неисправности",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    FaultEntry *entries = (FaultEntry *)calloc(MAX_FAULT_ENTRIES, sizeof(FaultEntry));
    if (!entries) {
        MessageBoxW(hwnd, L"Не хватило памяти.", L"Неисправности", MB_OK | MB_ICONWARNING);
        return;
    }
    int n_entries = 0;

    for (int i = 0; i < g_all_rows_count && n_entries < MAX_FAULT_ENTRIES; i++) {
        TableRow *row = &g_all_rows[i];
        if (!row->text[0])
            continue;

        char text_utf8[1200];
        wide_to_utf8(row->text, text_utf8, sizeof(text_utf8));

        CfdiuFault fault;
        if (!cfdiu_fault_parse(text_utf8, &fault))
            continue;

        int dup = 0;
        for (int j = 0; j < n_entries; j++) {
            if (reg_matches(entries[j].addr, row->addr) &&
                strcmp(entries[j].description, fault.description) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup)
            continue;

        FaultEntry *e = &entries[n_entries];
        reg_key(row->addr, e->addr);
        e->addr[15] = L'\0';
        wcsncpy(e->time, row->time, 31);
        e->time[31] = L'\0';
        safe_copy_local(e->severity, sizeof(e->severity), fault.severity);
        safe_copy_local(e->description, sizeof(e->description), fault.description);
        n_entries++;
    }

    if (n_entries == 0) {
        MessageBoxW(hwnd,
                     L"В принятых сообщениях не найдено отчётов о неисправностях\r\n"
                     L"(ищутся сообщения с меткой H1, начинающиеся с \"#CFB\" - формат\r\n"
                     L"CFDIU). Это нормально, если таких сообщений просто не было.",
                     L"Неисправности", MB_OK | MB_ICONINFORMATION);
        free(entries);
        return;
    }

    WCHAR *buf = (WCHAR *)malloc(sizeof(WCHAR) * (size_t)(4096 + n_entries * 260));
    if (!buf) {
        free(entries);
        MessageBoxW(hwnd, L"Не хватило памяти.", L"Неисправности", MB_OK | MB_ICONWARNING);
        return;
    }
    int pos = wsprintfW(buf, L"Найденные отчёты CFDIU за сессию (%d):\r\n\r\n", n_entries);

    for (int i = 0; i < n_entries; i++) {
        FaultEntry *e = &entries[i];
        WCHAR *sev_w = utf8_to_wide(strcmp(e->severity, "WRN") == 0 ? "предупреждение" : "отказ");
        WCHAR *desc_w = utf8_to_wide(e->description);
        pos += wsprintfW(buf + pos, L"%s  [%s]  [%s]  %s\r\n", e->addr, e->time,
                          sev_w ? sev_w : L"?", desc_w ? desc_w : L"?");
        free(sev_w);
        free(desc_w);
    }

    pos += wsprintfW(buf + pos,
                      L"\r\n(точное значение внутреннего кода/даты неисправности не "
                      L"подтверждено,\r\nпоказан только сам текст - см. README)");

    show_text_window(hwnd, L"Неисправности", buf);
    free(buf);
    free(entries);
}

/* Показывает найденные за сессию маршруты (борт: аэропорт вылета ->
 * назначения), собранные из текста уже накопленных сообщений (см.
 * g_all_rows) - используя ooi_parse() (формат "FI ...", роли полей
 * ИЗВЕСТНЫ) и, если он не сработал, airport_db_scan_pair() как более
 * осторожный резерв (роли НЕ известны - явно помечено). Тот же
 * простой формат "разовый снимок по запросу", что и у статистики/
 * диагностики выше. */
typedef struct {
    WCHAR addr[16];
    WCHAR time[32];
    char dep_icao[8], dest_icao[8];
    char dep_muni[80], dest_muni[80];
    int confirmed; /* 1 = роли (вылет/назначение) достоверно известны */
} RouteEntry;

#define MAX_ROUTE_ENTRIES 200

static void show_routes(HWND hwnd) {
    if (g_all_rows_count == 0) {
        MessageBoxW(hwnd, L"Пока не принято ни одного сообщения.", L"Маршруты",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    RouteEntry *entries = (RouteEntry *)calloc(MAX_ROUTE_ENTRIES, sizeof(RouteEntry));
    if (!entries) {
        MessageBoxW(hwnd, L"Не хватило памяти.", L"Маршруты", MB_OK | MB_ICONWARNING);
        return;
    }
    int n_entries = 0;

    for (int i = 0; i < g_all_rows_count && n_entries < MAX_ROUTE_ENTRIES; i++) {
        TableRow *row = &g_all_rows[i];
        if (!row->text[0])
            continue;

        char text_utf8[1200];
        wide_to_utf8(row->text, text_utf8, sizeof(text_utf8));

        char dep_icao[8] = "", dest_icao[8] = "";
        int confirmed = 0;

        OoiInfo ooi;
        if (ooi_parse(text_utf8, &ooi) && (ooi.departure[0] || ooi.destination[0])) {
            confirmed = 1;
            if (ooi.departure[0])
                safe_copy_local(dep_icao, sizeof(dep_icao), ooi.departure);
            if (ooi.destination[0])
                safe_copy_local(dest_icao, sizeof(dest_icao), ooi.destination);
        } else {
            AirportInfo a, b;
            if (airport_db_scan_pair(text_utf8, &a, &b)) {
                safe_copy_local(dep_icao, sizeof(dep_icao), a.icao);
                safe_copy_local(dest_icao, sizeof(dest_icao), b.icao);
            }
        }

        if (!dep_icao[0] && !dest_icao[0])
            continue;

        /* Дедупликация - не показываем один и тот же маршрут этого же
         * борта повторно, если он встретился в нескольких сообщениях. */
        int dup = 0;
        for (int j = 0; j < n_entries; j++) {
            if (reg_matches(entries[j].addr, row->addr) &&
                strcmp(entries[j].dep_icao, dep_icao) == 0 &&
                strcmp(entries[j].dest_icao, dest_icao) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup)
            continue;

        RouteEntry *e = &entries[n_entries];
        reg_key(row->addr, e->addr);
        e->addr[15] = L'\0';
        wcsncpy(e->time, row->time, 31);
        e->time[31] = L'\0';
        safe_copy_local(e->dep_icao, sizeof(e->dep_icao), dep_icao);
        safe_copy_local(e->dest_icao, sizeof(e->dest_icao), dest_icao);
        AirportInfo ap;
        if (dep_icao[0] && airport_db_lookup(dep_icao, &ap))
            safe_copy_local(e->dep_muni, sizeof(e->dep_muni), ap.municipality);
        if (dest_icao[0] && airport_db_lookup(dest_icao, &ap))
            safe_copy_local(e->dest_muni, sizeof(e->dest_muni), ap.municipality);
        e->confirmed = confirmed;
        n_entries++;
    }

    if (n_entries == 0) {
        MessageBoxW(hwnd,
                     L"Ни в одном сообщении не удалось распознать маршрут (ни формат\r\n"
                     L"\"FI ...\", ни пара кодов ICAO подряд в тексте). Это нормально - не\r\n"
                     L"все авиакомпании передают маршрут в узнаваемом виде, см. README.",
                     L"Маршруты", MB_OK | MB_ICONINFORMATION);
        free(entries);
        return;
    }

    WCHAR *buf = (WCHAR *)malloc(sizeof(WCHAR) * (size_t)(4096 + n_entries * 200));
    if (!buf) {
        free(entries);
        MessageBoxW(hwnd, L"Не хватило памяти.", L"Маршруты", MB_OK | MB_ICONWARNING);
        return;
    }
    int pos = wsprintfW(buf, L"Найденные маршруты за сессию (%d):\r\n\r\n", n_entries);

    for (int i = 0; i < n_entries; i++) {
        RouteEntry *e = &entries[i];
        WCHAR *dep_muni_w = e->dep_muni[0] ? utf8_to_wide(e->dep_muni) : NULL;
        WCHAR *dest_muni_w = e->dest_muni[0] ? utf8_to_wide(e->dest_muni) : NULL;
        WCHAR *dep_icao_w = utf8_to_wide(e->dep_icao[0] ? e->dep_icao : "?");
        WCHAR *dest_icao_w = utf8_to_wide(e->dest_icao[0] ? e->dest_icao : "?");

        pos += wsprintfW(buf + pos, L"%s  [%s]  %s%s%s%s \u2192 %s%s%s%s%s\r\n", e->addr, e->time,
                          dep_icao_w ? dep_icao_w : L"?", dep_muni_w ? L" (" : L"",
                          dep_muni_w ? dep_muni_w : L"", dep_muni_w ? L")" : L"",
                          dest_icao_w ? dest_icao_w : L"?", dest_muni_w ? L" (" : L"",
                          dest_muni_w ? dest_muni_w : L"", dest_muni_w ? L")" : L"",
                          e->confirmed ? L"" : L"  (порядок вылет/назначение не подтверждён)");
        free(dep_muni_w);
        free(dest_muni_w);
        free(dep_icao_w);
        free(dest_icao_w);
    }

    show_text_window(hwnd, L"Маршруты", buf);
    free(buf);
    free(entries);
}

/* Показывает сводку по уже накопленным в g_all_rows сообщениям: всего
 * сообщений, самые частые метки, самые частые борта (топ-10 каждого).
 * Простой снимок "по запросу" (обычный MessageBox), а не отдельное
 * постоянно обновляемое окно - для разовой сводки по сессии этого
 * вполне достаточно и не добавляет риска в виде ещё одного окна с
 * собственным жизненным циклом. */
static void show_statistics(HWND hwnd) {
    if (g_all_rows_count == 0) {
        MessageBoxW(hwnd, L"Пока не принято ни одного сообщения.", L"Статистика сессии",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    StatEntry *label_stats = (StatEntry *)malloc(sizeof(StatEntry) * MAX_STAT_KEYS);
    StatEntry *addr_stats = (StatEntry *)malloc(sizeof(StatEntry) * MAX_STAT_KEYS);
    if (!label_stats || !addr_stats) {
        free(label_stats);
        free(addr_stats);
        MessageBoxW(hwnd, L"Не хватило памяти для подсчёта статистики.", L"Статистика сессии",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    int n_labels = 0, n_addrs = 0;

    for (int i = 0; i < g_all_rows_count; i++) {
        stat_add(label_stats, &n_labels, g_all_rows[i].label);
        stat_add(addr_stats, &n_addrs, g_all_rows[i].addr);
    }

    qsort(label_stats, (size_t)n_labels, sizeof(StatEntry), stat_compare_desc);
    qsort(addr_stats, (size_t)n_addrs, sizeof(StatEntry), stat_compare_desc);

    /* Собираем итоговый текст сами (не через wsprintfW построчно с
     * ручным контролем позиции - легко ошибиться с размером буфера на
     * длинных обогащённых строках борта) - используем динамический
     * буфер помещающегося размера. */
    size_t cap = 4096 + (size_t)(n_labels > 10 ? 10 : n_labels) * 96 +
                 (size_t)(n_addrs > 10 ? 10 : n_addrs) * 300;
    WCHAR *buf = (WCHAR *)malloc(cap * sizeof(WCHAR));
    if (!buf) {
        free(label_stats);
        free(addr_stats);
        MessageBoxW(hwnd, L"Не хватило памяти для подсчёта статистики.", L"Статистика сессии",
                    MB_OK | MB_ICONWARNING);
        return;
    }

    int pos = wsprintfW(buf, L"Всего достоверных сообщений за сессию: %d\r\n"
                              L"Уникальных меток: %d, уникальных бортов: %d\r\n\r\n"
                              L"Самые частые метки:\r\n",
                         g_all_rows_count, n_labels, n_addrs);
    int top_labels = n_labels < 10 ? n_labels : 10;
    for (int i = 0; i < top_labels; i++)
        pos += wsprintfW(buf + pos, L"  %s  %d\r\n", label_stats[i].key, label_stats[i].count);

    pos += wsprintfW(buf + pos, L"\r\nСамые частые борта:\r\n");
    int top_addrs = n_addrs < 10 ? n_addrs : 10;
    for (int i = 0; i < top_addrs; i++)
        pos += wsprintfW(buf + pos, L"  %s  \xe2\x80\x94  %d\r\n", addr_stats[i].key, addr_stats[i].count);

    show_text_window(hwnd, L"Статистика сессии", buf);

    free(buf);
    free(label_stats);
    free(addr_stats);
}

/* ---------- проверка окружения ----------
 * Одним нажатием собирает то, что обычно приходится выяснять по
 * переписке шаг за шагом при любой странности с настройкой: рабочую
 * папку программы (именно относительно неё ищутся aircraft_db.csv и
 * создаётся acars_logs - самый частый источник путаницы "файл же на
 * месте!"), список устройств записи, статус AR5000 и статус базы
 * бортов. */
static void check_environment(HWND hwnd) {
    WCHAR buf[8192];
    int pos = 0;

    WCHAR cwd[512];
    DWORD cwd_len = GetCurrentDirectoryW(512, cwd);
    if (cwd_len == 0 || cwd_len >= 512)
        wcscpy(cwd, L"(не удалось определить)");

    pos += wsprintfW(buf + pos,
                      L"Рабочая папка программы (именно тут программа ищет aircraft_db.csv\r\n"
                      L"и создаёт папку acars_logs - если что-то не находится, в первую\r\n"
                      L"очередь проверяйте, что нужный файл лежит ИМЕННО здесь):\r\n  %s\r\n\r\n",
                      cwd);

    int n_dev = get_audio_device_count();
    pos += wsprintfW(buf + pos, L"Устройства записи звука: %d\r\n", n_dev);
    int shown_dev = n_dev < 10 ? n_dev : 10;
    for (int i = 0; i < shown_dev; i++) {
        char name[256];
        if (get_audio_device_name(i, name, sizeof(name)) == 0) {
            WCHAR *w = ansi_to_wide(name);
            pos += wsprintfW(buf + pos, L"  [%d] %s\r\n", i, w ? w : L"?");
            free(w);
        } else {
            pos += wsprintfW(buf + pos, L"  [%d] (не удалось получить имя)\r\n", i);
        }
    }
    if (n_dev == 0)
        pos += wsprintfW(buf + pos, L"  (не найдено ни одного - проверьте подключение микрофона/приёмника)\r\n");
    pos += wsprintfW(buf + pos, L"\r\n");

    if (ar5000_is_connected())
        pos += wsprintfW(buf + pos, L"AR5000: подключен\r\n\r\n");
    else
        pos += wsprintfW(buf + pos, L"AR5000: не подключен\r\n\r\n");

    if (aircraft_db_count() > 0) {
        pos += wsprintfW(buf + pos, L"База бортов (aircraft_db.csv): загружено %d записей\r\n\r\n",
                          aircraft_db_count());
    } else if (aircraft_db_last_warning()) {
        WCHAR *w = utf8_to_wide(aircraft_db_last_warning());
        pos += wsprintfW(buf + pos, L"База бортов (aircraft_db.csv): %s\r\n\r\n", w ? w : L"?");
        free(w);
    } else {
        pos += wsprintfW(buf + pos,
                          L"База бортов (aircraft_db.csv): файл не найден в рабочей папке выше -\r\n"
                          L"это необязательная функция, ничего не сломано, просто обогащение\r\n"
                          L"борта типом/авиакомпанией работать не будет.\r\n\r\n");
    }

    if (airport_db_count() > 0) {
        pos += wsprintfW(buf + pos, L"База аэропортов (airport_db.csv): загружено %d записей\r\n\r\n",
                          airport_db_count());
    } else if (airport_db_last_warning()) {
        WCHAR *w = utf8_to_wide(airport_db_last_warning());
        pos += wsprintfW(buf + pos, L"База аэропортов (airport_db.csv): %s\r\n\r\n", w ? w : L"?");
        free(w);
    } else {
        pos += wsprintfW(buf + pos,
                          L"База аэропортов (airport_db.csv): файл не найден - необязательно,\r\n"
                          L"коды аэропортов в маршрутах не будут расшифрованы в названия.\r\n\r\n");
    }

    pos += wsprintfW(buf + pos, L"Папка истории сообщений: %s\\acars_logs\r\n"
                                 L"(создаётся автоматически при первом достоверно принятом сообщении)",
                      cwd);

    show_text_window(hwnd, L"Проверка окружения", buf);
}

/* Окно банков памяти (memory_view.c): чтение банков, таблица каналов, журнал обмена, настройка на канал.
 * Когда в нём настроили приёмник на канал, подставляем частоту и режим в поля главного окна. */
static void on_memory_tuned(uint32_t hz, int mode) {
    if (hz) {
        WCHAR buf[32];
        format_mhz(buf, 32, hz / 1000000.0);
        SetWindowTextW(g_hEditFreq, buf);
    }
    if (mode >= 0 && mode < 8)
        SendMessageW(g_hComboMode, CB_SETCURSEL, (WPARAM)mode, 0);
}

static void show_memory_list(HWND hwnd) {
    int bank = (int)SendMessageW(g_hComboMemBank, CB_GETCURSEL, 0, 0);
    memview_show(hwnd, bank, on_memory_tuned);
}

static int CALLBACK table_compare(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort) {
    (void)lParamSort;
    WCHAR buf1[540], buf2[540];
    ListView_GetItemText(g_hTable, (int)lParam1, g_sort_column, buf1, 540);
    ListView_GetItemText(g_hTable, (int)lParam2, g_sort_column, buf2, 540);
    int cmp = wcscmp(buf1, buf2);
    return g_sort_ascending ? cmp : -cmp;
}

static void set_status(const WCHAR *text, COLORREF color) {
    WCHAR buf[256];
    wsprintfW(buf, L"\u25CF %s", text);
    g_status_color = color;
    SetWindowTextW(g_hStatus, buf);
    InvalidateRect(g_hStatus, NULL, TRUE);
}

/* ---------- перенаправление stdout в окно вывода ---------- */

/* Подбирает цвет для строки лога по её содержимому - простая
 * подсветка "как в терминале", без полноценного парсинга: строки
 * печатаются функцией print_message()/print_message_live() всегда в
 * одном и том же формате (см. main.c/audio_win.c), так что подстрок
 * достаточно. */
static COLORREF pick_line_color(const WCHAR *line) {
    if (wcsstr(line, L"CRC: OK"))
        return CLR_FG_OK;
    if (wcsstr(line, L"ОШИБКА"))
        return CLR_FG_ERR;
    if (wcsstr(line, L"---------"))
        return CLR_FG_DIM;
    if (wcsstr(line, L"  Mode") || wcsstr(line, L"  Addr") || wcsstr(line, L"  Ack") ||
        wcsstr(line, L"  Label") || wcsstr(line, L"  Block") || wcsstr(line, L"  Suffix"))
        return CLR_FG_FIELD;
    if (wcsstr(line, L"  Text"))
        return CLR_FG_TEXT;
    if (wcsstr(line, L"Файл:") || wcsstr(line, L"Средняя частота") ||
        wcsstr(line, L"Сквелч") || wcsstr(line, L"Всплеск") || wcsstr(line, L"Захват:") ||
        wcsstr(line, L"Готово.") || wcsstr(line, L"Устройства записи"))
        return CLR_FG_DIM;
    return CLR_FG_DEFAULT;
}

static void append_log(const WCHAR *text) {
    int len = GetWindowTextLengthW(g_hEditLog);
    SendMessageW(g_hEditLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);

    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = pick_line_color(text);
    SendMessageW(g_hEditLog, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

    SendMessageW(g_hEditLog, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageW(g_hEditLog, EM_SCROLLCARET, 0, 0);
}

static DWORD WINAPI pipe_reader_thread_proc(LPVOID param) {
    (void)param;
    char buf[4096];
    static char line_accum[16384];
    size_t accum_len = 0;
    DWORD n;

    while (ReadFile(g_stdout_read, buf, sizeof(buf), &n, NULL) && n > 0) {
        for (DWORD i = 0; i < n; i++) {
            if (accum_len < sizeof(line_accum) - 1)
                line_accum[accum_len++] = buf[i];
            if (buf[i] == '\n') {
                line_accum[accum_len] = '\0';
                WCHAR *w = utf8_to_wide(line_accum);
                if (w)
                    PostMessageW(g_hwnd_main, WM_APP_LOG, 0, (LPARAM)w);
                accum_len = 0;
            }
        }
    }
    return 0;
}

static void setup_stdout_redirect(void) {
    HANDLE read_h = NULL, write_h = NULL;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&read_h, &write_h, &sa, 0))
        return;

    /* Классический вариант с _dup2(fd, _fileno(stdout)) на практике
     * иногда не подхватывается некоторыми сборками CRT MinGW для уже
     * использованного потока (например, если stderr к этому моменту
     * уже что-то печатал) - более надёжный, широко задокументированный
     * способ для Windows-приложений - напрямую подменить содержимое
     * структуры FILE*, на которую указывают stdout/stderr, содержимым
     * новой, открытой поверх нашего pipe. Каждому потоку - свой
     * отдельный fd поверх ОДНОГО и того же HANDLE (сам HANDLE можно
     * открывать как fd многократно). */
    int fd_out = _open_osfhandle((intptr_t)write_h, _O_WRONLY | _O_TEXT);
    int fd_err = _open_osfhandle((intptr_t)write_h, _O_WRONLY | _O_TEXT);
    if (fd_out >= 0) {
        FILE *fp_out = _fdopen(fd_out, "w");
        if (fp_out)
            *stdout = *fp_out;
    }
    if (fd_err >= 0) {
        FILE *fp_err = _fdopen(fd_err, "w");
        if (fp_err)
            *stderr = *fp_err;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    g_stdout_read = read_h;
    CreateThread(NULL, 0, pipe_reader_thread_proc, NULL, 0, NULL);
}

/* ---------- рабочие потоки (приём с устройства / декодирование файла) ---------- */

static DWORD WINAPI capture_thread_proc(LPVOID param) {
    int device = (int)(intptr_t)param;
    run_live_capture(device, &g_stop_flag);
    PostMessageW(g_hwnd_main, WM_APP_CAPTURE_STOPPED, 0, 0);
    return 0;
}

static DWORD WINAPI file_decode_thread_proc(LPVOID param) {
    char *path = (char *)param;
    decode_wav_file(path, 0, 0);
    free(path);
    PostMessageW(g_hwnd_main, WM_APP_FILE_DONE, 0, 0);
    return 0;
}

/* ---------- AR5000: опрос уровня сигнала (S-метр) в фоновом потоке ----------
 * ar5000_get_signal_level() делает блокирующий обмен по COM-порту
 * (до ~400 мс тайм-аута) - гонять его прямо в обработчике WM_TIMER
 * означало бы подвешивать окно на это время каждую секунду. Вместо
 * этого - отдельный поток, как и для живого приёма/декодирования
 * файла: он же сам решает, когда опрашивать (раз в секунду), и
 * передаёт результат в GUI-поток через PostMessage. */
static DWORD WINAPI ar_poll_thread_proc(LPVOID param) {
    (void)param;
    while (!g_ar_poll_stop) {
        int level = -1, squelch_open = 0;
        if (ar5000_get_signal_level(&level, &squelch_open) != 0)
            level = -1; /* приёмник не ответил - покажем как "нет данных" */
        PostMessageW(g_hwnd_main, WM_APP_AR_STATUS, (WPARAM)level, (LPARAM)squelch_open);

        for (int waited = 0; waited < 1000 && !g_ar_poll_stop; waited += 100)
            Sleep(100);
    }
    return 0;
}

static void ar_start_polling(void) {
    g_ar_poll_stop = 0;
    g_ar_poll_thread = CreateThread(NULL, 0, ar_poll_thread_proc, NULL, 0, NULL);
}

static void ar_stop_polling(void) {
    if (g_ar_poll_thread) {
        g_ar_poll_stop = 1;
        WaitForSingleObject(g_ar_poll_thread, 2000);
        CloseHandle(g_ar_poll_thread);
        g_ar_poll_thread = NULL;
    }
}

/* ---------- AR5000: автосканирование частот ----------
 * Перебирает список частых частот ACARS (kAcarsPresets): на каждой
 * недолго слушает (DWELL_MS) - если за это время шумоподавитель
 * открылся (кто-то реально передаёт на этой частоте), задерживается
 * дольше (LISTEN_MS), чтобы дать декодеру шанс поймать пакет; если нет
 * - сразу переходит к следующей частоте. Сам приём/декодирование не
 * трогает вообще - это отдельный, уже работающий конвейер (живой
 * приём со звуковой карты); сканер только переключает то, НА ЧТО
 * настроен приёмник, а что бы ни пришло по звуку - decoder разберётся
 * сам, независимо от того, что происходит на COM-порту. */
#define SCAN_DWELL_MS 3000
#define SCAN_LISTEN_MS 15000

static int scan_sleep_checked(int ms) {
    for (int waited = 0; waited < ms; waited += 200) {
        if (g_scan_stop)
            return 1; /* прервано */
        Sleep(200);
    }
    return g_scan_stop;
}

static DWORD WINAPI scan_thread_proc(LPVOID param) {
    (void)param;
    while (!g_scan_stop) {
        for (int i = 0; i < N_ACARS_PRESETS && !g_scan_stop; i++) {
            double mhz = kAcarsPresets[i];
            g_scan_current_mhz = mhz;
            uint32_t hz = (uint32_t)(mhz * 1000000.0 + 0.5);
            ar5000_set_frequency_hz(hz);

            int found = 0;
            int elapsed = 0;
            while (elapsed < SCAN_DWELL_MS && !g_scan_stop) {
                int level = 0, squelch_open = 0;
                if (ar5000_get_signal_level(&level, &squelch_open) == 0 && squelch_open) {
                    found = 1;
                    break;
                }
                if (scan_sleep_checked(250))
                    break;
                elapsed += 250;
            }
            if (found)
                scan_sleep_checked(SCAN_LISTEN_MS);
        }
    }
    return 0;
}

static void scan_start(void) {
    g_scan_stop = 0;
    g_scan_active = 1;
    g_scan_thread = CreateThread(NULL, 0, scan_thread_proc, NULL, 0, NULL);
}

static void scan_stop(void) {
    if (g_scan_thread) {
        g_scan_stop = 1;
        WaitForSingleObject(g_scan_thread, 3000);
        CloseHandle(g_scan_thread);
        g_scan_thread = NULL;
    }
    g_scan_active = 0;
}

static int get_selected_baud(void) {
    WCHAR buf[16];
    GetWindowTextW(g_hComboBaud, buf, 16);
    return _wtoi(buf);
}

static void ar_do_connect(HWND hwnd) {
    char com[32];
    WCHAR wcom[32];
    GetWindowTextW(g_hEditCom, wcom, 32);
    WideCharToMultiByte(CP_UTF8, 0, wcom, -1, com, sizeof(com), NULL, NULL);
    if (com[0] == '\0') {
        MessageBoxW(hwnd, L"Укажите COM-порт (например, COM3).", L"Внимание", MB_OK | MB_ICONWARNING);
        return;
    }
    int baud = get_selected_baud();

    if (ar5000_connect(com, baud) != 0) {
        WCHAR msg[256];
        wsprintfW(msg, L"Не удалось открыть %s на %d бод.\r\n"
                        L"Проверьте, что порт указан верно, не занят другой программой,\r\n"
                        L"и что скорость совпадает с CONFIG-меню самого приёмника.",
                  wcom, baud);
        MessageBoxW(hwnd, msg, L"AR5000 - ошибка подключения", MB_OK | MB_ICONERROR);
        return;
    }

    /* Проверяем связь запросом версии прошивки - если приёмник не
     * ответил, порт открылся, но это, возможно, не AR5000 вовсе (или
     * он выключен/не в REMOTE-режиме). Всё равно оставляем соединение
     * открытым - AR5000 переходит в REMOTE автоматически при получении
     * первой же команды, так что установка частоты чуть ниже должна
     * сработать, даже если VR не ответил вовремя. */
    char version[64] = "";
    int have_version = (ar5000_get_version(version, sizeof(version)) == 0);

    /* Режим/полоса - те, что сейчас выбраны в комбобоксах (по умолчанию
     * AM/6кГц - то, что нужно для ACARS, но если пользователь заранее
     * выбрал что-то другое перед подключением - применяем его выбор, а
     * не молча переопределяем на своё). */
    int mode_sel = (int)SendMessageW(g_hComboMode, CB_GETCURSEL, 0, 0);
    int bw_sel = (int)SendMessageW(g_hComboBW, CB_GETCURSEL, 0, 0);
    ar5000_set_mode(mode_sel >= 0 ? mode_sel : 1);
    ar5000_set_bandwidth(bw_sel >= 0 ? bw_sel : 2);

    ar_start_polling();

    EnableWindow(g_hEditCom, FALSE);
    EnableWindow(g_hComboBaud, FALSE);
    SetWindowTextW(g_hBtnArConnect, L"Отключить");

    WCHAR statusbuf[200];
    if (have_version) {
        WCHAR *wver = utf8_to_wide(version);
        wsprintfW(statusbuf, L"\u25CF AR5000: подключен (%s), %S @ %d бод", wver ? wver : L"?", com, baud);
        free(wver);
        g_ar_status_color = CLR_FG_OK;
    } else {
        wsprintfW(statusbuf, L"\u25CF AR5000: порт открыт (%S @ %d бод), но приёмник не ответил на запрос версии",
                  com, baud);
        g_ar_status_color = CLR_FG_TEXT;
    }
    SetWindowTextW(g_hArStatus, statusbuf);
    InvalidateRect(g_hArStatus, NULL, TRUE);
}

static void ar_do_disconnect(void) {
    if (g_scan_active) {
        scan_stop();
        SetWindowTextW(g_hBtnScan, L"\u21BB Сканировать");
        EnableWindow(g_hEditFreq, TRUE);
        EnableWindow(g_hBtnTune, TRUE);
        EnableWindow(g_hComboPreset, TRUE);
    }
    ar_stop_polling();
    ar5000_disconnect();
    EnableWindow(g_hEditCom, TRUE);
    EnableWindow(g_hComboBaud, TRUE);
    SetWindowTextW(g_hBtnArConnect, L"Подключить");
    g_ar_status_color = CLR_FG_DIM;
    SetWindowTextW(g_hArStatus, L"\u25CB AR5000: не подключен");
    InvalidateRect(g_hArStatus, NULL, TRUE);
    if (g_hArProgress)
        SendMessageW(g_hArProgress, PBM_SETPOS, 0, 0);
}

static void ar_do_tune(HWND hwnd) {
    if (!ar5000_is_connected()) {
        MessageBoxW(hwnd, L"Сначала подключитесь к приёмнику.", L"Внимание", MB_OK | MB_ICONWARNING);
        return;
    }
    WCHAR wfreq[32];
    GetWindowTextW(g_hEditFreq, wfreq, 32);
    double mhz = _wtof(wfreq);
    if (mhz < 0.01 || mhz > 3300.0) { /* заявленный диапазон AR5000: 10 кГц - 3.3 ГГц */
        MessageBoxW(hwnd, L"Некорректная частота. Укажите значение в МГц (например, 131.725).",
                    L"Внимание", MB_OK | MB_ICONWARNING);
        return;
    }
    uint32_t hz = (uint32_t)(mhz * 1000000.0 + 0.5);
    ar5000_set_frequency_hz(hz);
}

/* ---------- заполнение списка устройств ---------- */

static void refresh_device_list(void) {
    SendMessageW(g_hCombo, CB_RESETCONTENT, 0, 0);
    int n = get_audio_device_count();
    for (int i = 0; i < n; i++) {
        char name[256];
        WCHAR label[300];
        if (get_audio_device_name(i, name, sizeof(name)) == 0) {
            WCHAR *w = ansi_to_wide(name);
            wsprintfW(label, L"[%d] %s", i, w ? w : L"?");
            free(w);
        } else {
            wsprintfW(label, L"[%d] (?)", i);
        }
        SendMessageW(g_hCombo, CB_ADDSTRING, 0, (LPARAM)label);
    }
    if (n > 0)
        SendMessageW(g_hCombo, CB_SETCURSEL, 0, 0);
    if (n == 0)
        append_log(L"Устройства записи не найдены - проверьте подключение звуковой карты/приёмника.\r\n");
}

/* ---------- диалог выбора файла ---------- */

/* --- Импорт ранее сохранённого журнала (acars_log.jsonl) обратно в
 * программу --- Полезно, когда исходной звукозаписи уже нет (или
 * просто не хочется декодировать заново), а посмотреть маршруты/
 * телеметрию/погоду/неисправности из уже готового журнала хочется -
 * все кнопки-отчёты (включая "Карта") работают с g_all_rows, а не
 * напрямую со звуком, так что заполнить его из журнала - то же самое,
 * что заново его "принять". Пишем СВОЙ, узко заточенный под именно
 * НАШ СОБСТВЕННЫЙ формат JSON-парсер (см. json_escape() в
 * history_log.c - экранирует ровно \" \\ \n \r \t и \u00XX) - не
 * универсальный, но для наших же файлов этого достаточно, и не нужна
 * внешняя библиотека. */

/* Находит "key":"значение" в строке JSON и копирует РАСЭКРАНИРОВАННОЕ
 * значение в out. Возвращает 1 при успехе (ключ найден), 0 если ключа
 * в строке нет вовсе (тогда out не трогается). */
static int json_extract_string(const char *line, const char *key, char *out, size_t out_size) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    /* Ищем  "ключ" : "значение"  - пробелы вокруг двоеточия допускаются: наш собственный журнал компактный
     * ("ключ":"значение"), но тот же файл, пересохранённый другим инструментом, часто бывает с пробелами.
     * Значение обязано быть строкой; если после ключа стоит что-то другое (число, null), ищем дальше. */
    const char *p = line;
    for (;;) {
        p = strstr(p, pattern);
        if (!p)
            return 0;
        const char *q = p + strlen(pattern);
        while (*q == ' ' || *q == '\t')
            q++;
        if (*q == ':') {
            q++;
            while (*q == ' ' || *q == '\t')
                q++;
            if (*q == '"') {
                p = q + 1; /* сразу за открывающей кавычкой значения */
                break;
            }
        }
        p++;
    }

    size_t o = 0;
    while (*p && *p != '"' && o < out_size - 1) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': out[o++] = '\n'; break;
            case 'r': out[o++] = '\r'; break;
            case 't': out[o++] = '\t'; break;
            case '"': out[o++] = '"'; break;
            case '\\': out[o++] = '\\'; break;
            case '/': out[o++] = '/'; break;
            case 'u': {
                /* \u00XX - у нас такими кодируются только управляющие
                 * символы ниже 0x20 (см. json_escape) - 4 шестнадцатеричные
                 * цифры, нам из них нужен только младший байт */
                if (p[1] && p[2] && p[3] && p[4]) {
                    char hex[3] = {p[3], p[4], '\0'};
                    out[o++] = (char)strtol(hex, NULL, 16);
                    p += 4;
                }
                break;
            }
            default: out[o++] = *p; break;
            }
            p++;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
    return 1;
}

/* ---- Защита от повторов -------------------------------------------------
 * Одно и то же сообщение не должно попасть в память дважды: ни при повторном нажатии "Импорт JSONL...", ни
 * когда журнал лежит сразу в двух местах (acars_logs\acars_log.jsonl и его копия рядом с программой).
 * Сообщение узнаётся по времени, адресу борта, метке и тексту - сводится к 64-битному хэшу (FNV-1a), хэши
 * хранятся в открытой хэш-таблице. */
typedef struct {
    unsigned long long *slot; /* 0 - пустая ячейка (настоящий хэш 0 заменяется на 1) */
    size_t cap, n;
} SeenSet;

static unsigned long long row_hash(const TableRow *r) {
    unsigned long long h = 1469598103934665603ULL;
    const WCHAR *parts[4] = {r->time, r->addr, r->label, r->text};
    for (int k = 0; k < 4; k++) {
        for (const WCHAR *p = parts[k]; *p; p++) {
            h ^= (unsigned long long)*p;
            h *= 1099511628211ULL;
        }
        h ^= 0x1F; /* разделитель полей: "AB"+"C" и "A"+"BC" - разные сообщения */
        h *= 1099511628211ULL;
    }
    return h ? h : 1ULL;
}

static void seen_free(SeenSet *s) {
    free(s->slot);
    s->slot = NULL;
    s->cap = s->n = 0;
}

/* 1 - хэш добавлен, 0 - такой уже был (повтор), -1 - не хватило памяти (тогда повторы не отсеиваются). */
static int seen_add(SeenSet *s, unsigned long long h) {
    if (s->n * 2 >= s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 1024;
        unsigned long long *ns = (unsigned long long *)calloc(ncap, sizeof(*ns));
        if (!ns)
            return -1;
        for (size_t i = 0; i < s->cap; i++) {
            if (s->slot[i]) {
                size_t j = (size_t)(s->slot[i] & (ncap - 1));
                while (ns[j])
                    j = (j + 1) & (ncap - 1);
                ns[j] = s->slot[i];
            }
        }
        free(s->slot);
        s->slot = ns;
        s->cap = ncap;
    }
    size_t i = (size_t)(h & (s->cap - 1));
    while (s->slot[i]) {
        if (s->slot[i] == h)
            return 0;
        i = (i + 1) & (s->cap - 1);
    }
    s->slot[i] = h;
    s->n++;
    return 1;
}

/* Заносит в набор всё, что уже лежит в памяти (принятое вживую или загруженное раньше). */
static void seen_from_rows(SeenSet *s) {
    for (int i = 0; i < g_all_rows_count; i++)
        seen_add(s, row_hash(&g_all_rows[i]));
}

/* Одна строка журнала (JSON) -> строка таблицы. 0 - строка не нашего формата (нет поля timestamp). */
static int jsonl_line_to_row(const char *line, TableRow *row) {
    char timestamp[64], source[256], mode[8], addr[32], label[16], label_desc[300], text[700];
    timestamp[0] = source[0] = mode[0] = addr[0] = label[0] = label_desc[0] = text[0] = '\0';

    int ok = json_extract_string(line, "timestamp", timestamp, sizeof(timestamp));
    json_extract_string(line, "source", source, sizeof(source));
    json_extract_string(line, "mode", mode, sizeof(mode));
    json_extract_string(line, "addr", addr, sizeof(addr));
    json_extract_string(line, "label", label, sizeof(label));
    json_extract_string(line, "label_description", label_desc, sizeof(label_desc));
    json_extract_string(line, "text", text, sizeof(text));

    if (!ok)
        return 0; /* нет даже timestamp - строка не похожа на наш формат (например, вообще не JSON) */
    /* Пустой адрес - НЕ признак чужого формата: так выглядят служебные сообщения наземных станций (метка SQ) -
     * у них в заголовке нет адреса борта. При живом приёме они показываются как "(?)" (см. gui_on_message). */
    if (!addr[0])
        safe_copy_local(addr, sizeof(addr), "(?)");

    memset(row, 0, sizeof(*row));
    WCHAR *w;
    w = utf8_to_wide(timestamp);
    if (w) { wcsncpy(row->time, w, 31); row->time[31] = L'\0'; free(w); }
    w = utf8_to_wide(source);
    if (w) { wcsncpy(row->source, w, 159); row->source[159] = L'\0'; free(w); }
    w = utf8_to_wide(mode);
    if (w) { wcsncpy(row->mode, w, 1); row->mode[1] = L'\0'; free(w); }
    w = utf8_to_wide(addr);
    if (w) { wcsncpy(row->addr, w, 219); row->addr[219] = L'\0'; free(w); }
    w = utf8_to_wide(label);
    if (w) { wcsncpy(row->label, w, 3); row->label[3] = L'\0'; free(w); }
    w = utf8_to_wide(label_desc);
    if (w) { wcsncpy(row->label_desc, w, 299); row->label_desc[299] = L'\0'; free(w); }
    w = utf8_to_wide(text);
    if (w) { wcsncpy(row->text, w, 539); row->text[539] = L'\0'; free(w); }
    return 1;
}

typedef struct {
    int imported;  /* принято в память */
    int skipped;   /* строки не нашего формата */
    int dups;      /* повторы - уже были в памяти */
    int tail_only; /* 1 - файл большой, прочитан только конец */
    int err;       /* при неудаче открытия - errno */
} JsonlStats;

/* В памяти хранятся только последние MAX_STORED_ROWS сообщений, поэтому из большого журнала читаем только
 * конец: так запуск не задерживается, сколько бы ни накопилось за месяцы (журнал только дополняется). */
#define JSONL_TAIL_BYTES (8L * 1024 * 1024)

/* Загружает сообщения из журнала path (UTF-8) в память. seen - набор уже известных сообщений (повторы
 * пропускаются; NULL - не проверять). Возвращает -1, если файл не открылся, иначе 0. Окон не показывает -
 * годится и для кнопки, и для автозагрузки при запуске. */
static int import_jsonl_path(const char *path, SeenSet *seen, JsonlStats *st) {
    memset(st, 0, sizeof(*st));
    FILE *f = fopen_utf8(path, "rb"); /* "rb", не "r" - переводами строк управляем сами; fopen_utf8 - потому что
                                       * путь из окна в UTF-8, а обычный fopen читал бы его как ANSI и не находил
                                       * файлы в папках с кириллицей */
    if (!f) {
        st->err = errno;
        return -1;
    }

    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        if (size > JSONL_TAIL_BYTES && fseek(f, size - JSONL_TAIL_BYTES, SEEK_SET) == 0) {
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {
            } /* первая строка после перемотки обрезана с начала - выбрасываем */
            st->tail_only = 1;
        } else {
            fseek(f, 0, SEEK_SET);
        }
    } else {
        fseek(f, 0, SEEK_SET);
    }

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strcspn(line, "\r\n");
        line[len] = '\0';
        size_t lead = 0;
        while (line[lead] == ' ' || line[lead] == '\t')
            lead++;
        if (line[lead] == '\0') /* пустая строка или только пробелы - не "мусор", просто пропускаем молча */
            continue;

        TableRow row;
        if (!jsonl_line_to_row(line, &row)) {
            st->skipped++;
            continue;
        }
        if (seen && seen_add(seen, row_hash(&row)) == 0) {
            st->dups++;
            continue;
        }
        store_row(&row);
        if (g_table_view) {
            WCHAR flt[256], acf[64];
            GetWindowTextW(g_hEditFilter, flt, 256);
            read_aircraft_filter(acf, 64);
            if (row_matches_filter(&row, flt) && row_matches_aircraft(&row, acf))
                table_add_row(&row);
        }
        st->imported++;
    }
    fclose(f);
    return 0;
}

static void remember_import_path(const char *path);

static void import_jsonl_file(HWND owner, const WCHAR *wpath) {
    char path[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, sizeof(path), NULL, NULL);

    SeenSet seen;
    memset(&seen, 0, sizeof(seen));
    seen_from_rows(&seen); /* то, что уже в памяти, второй раз не добавляется */
    JsonlStats st;
    int r = import_jsonl_path(path, &seen, &st);
    seen_free(&seen);
    if (r >= 0)
        remember_import_path(path); /* чтобы выгрузка и отчёты по борту видели и этот файл */
    if (r < 0) {
        /* показываем путь и код ошибки - чтобы причину (нет файла, нет доступа, файл занят) не приходилось гадать */
        WCHAR em[MAX_PATH + 300];
        wsprintfW(em, L"Не удалось открыть файл:\r\n%s\r\n\r\nКод ошибки: %d%s", wpath, st.err,
                  st.err == 2 ? L" (файл не найден)"
                  : st.err == 13 ? L" (нет доступа - файл занят другой программой или закрыт правами)"
                                 : L"");
        MessageBoxW(owner, em, L"Импорт журнала", MB_OK | MB_ICONWARNING);
        return;
    }

    WCHAR msg[900];
    wsprintfW(msg, L"Импортировано сообщений: %d\r\n", st.imported);
    if (st.dups > 0) {
        WCHAR t[160];
        wsprintfW(t, L"Повторы пропущены (уже были в памяти): %d\r\n", st.dups);
        wcscat(msg, t);
    }
    if (st.skipped > 0)
        wcscat(msg, L"Часть строк пропущена - не похожи на наш формат.\r\n");
    if (st.tail_only)
        wcscat(msg, L"Файл большой: прочитан только его конец (в памяти всё равно хранятся последние 5000 сообщений).\r\n");
    wcscat(msg,
           L"\r\nТеперь можно пользоваться кнопками-отчётами (Маршруты, Телеметрия,\r\n"
           L"Погода, Неисправности, Карта, Подробности) как обычно - они работают\r\n"
           L"с этими данными точно так же, как с только что принятыми.");
    MessageBoxW(owner, msg, L"Импорт журнала", MB_OK | MB_ICONINFORMATION);
}

/* Автозагрузка журнала при запуске: чтобы после старта сразу были борта на карте и работали кнопки-отчёты,
 * без "Импорт JSONL...". Читаются (по порядку, повторы между файлами отсеиваются):
 *   1) acars_logs\acars_log.jsonl - собственный журнал программы (в него пишется каждое принятое сообщение);
 *   2) acars_log.jsonl рядом с программой - если вы положили туда копию.
 * Отключается строкой AutoLoad=0 в разделе [Import] файла acars_settings.ini. Ничего не показывает, если
 * журнала нет; результат пишет в окно вывода. Папка - рабочая папка программы. */
static void get_settings_path(WCHAR *out, int out_size_chars); /* определена ниже */

static void autoload_journals(HWND hwnd) {
    (void)hwnd;
    WCHAR ini[600];
    get_settings_path(ini, 600);
    if (GetPrivateProfileIntW(L"Import", L"AutoLoad", 1, ini) == 0)
        return;

    static const WCHAR *const files[] = {L"acars_logs\\acars_log.jsonl", L"acars_log.jsonl"};
    SeenSet seen;
    memset(&seen, 0, sizeof(seen));
    seen_from_rows(&seen);

    int total = 0, dups = 0, found = 0;
    WCHAR report[900];
    report[0] = L'\0';
    for (int i = 0; i < 2; i++) {
        char path[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, files[i], -1, path, sizeof(path), NULL, NULL);
        JsonlStats st;
        if (import_jsonl_path(path, &seen, &st) < 0)
            continue; /* такого файла нет - это нормально */
        found++;
        total += st.imported;
        dups += st.dups;
        WCHAR piece[200];
        wsprintfW(piece, L"%s%s: %d%s", report[0] ? L";  " : L"", files[i], st.imported,
                  st.tail_only ? L" (конец файла)" : L"");
        wcscat(report, piece);
    }
    seen_free(&seen);
    if (!found || total == 0)
        return;

    WCHAR msg[1200];
    wsprintfW(msg, L"Загружен журнал, сообщений: %d  (%s)%s. Кнопки-отчёты и карта готовы.\r\n", total, report,
              dups > 0 ? L"; повторы между файлами пропущены" : L"");
    append_log(msg);
    set_status(L"Журнал загружен.", CLR_FG_LABEL);
}

/* Стандартный диалог "Открыть файл" САМ МЕНЯЕТ рабочую папку процесса на папку выбранного файла
 * (Microsoft: "диалог автоматически меняет текущую папку"). А от рабочей папки в программе зависит
 * многое: acars_settings.ini, acars_logs, кэш тайлов карты, папка aircraft_photos. Поэтому после
 * "Импорт JSONL..." или "Открыть WAV-файл..." всё это начинало бы искаться в папке файла. Флаг
 * OFN_NOCHANGEDIR на старых Windows (NT/2000/XP) не действует, потому папку ещё и запоминаем
 * до диалога и возвращаем после - так работает везде. */
static BOOL open_dialog_keep_cwd(OPENFILENAMEW *ofn) {
    WCHAR saved[MAX_PATH];
    saved[0] = L'\0';
    GetCurrentDirectoryW(MAX_PATH, saved);
    ofn->Flags |= OFN_NOCHANGEDIR;
    BOOL r = GetOpenFileNameW(ofn);
    if (saved[0])
        SetCurrentDirectoryW(saved);
    return r;
}

/* ---------- Откуда берутся данные для выгрузки и отчётов по борту ----------
 * 1) журнал программы (то, что принято с приёмника и расшифровано из WAV),
 * 2) файлы JSONL, которые вы загружали кнопкой «Импорт JSONL...» в этом сеансе,
 * 3) всё, что сейчас в памяти программы (запасной вариант: сообщения, которых нет ни в журнале, ни в этих файлах).
 * Повторы между источниками отбрасываются по содержимому (время, борт, метка, начало текста). */
#define MAX_IMPORT_PATHS 8
static char g_import_paths[MAX_IMPORT_PATHS][MAX_PATH * 3];
static int g_n_import_paths;

static void remember_import_path(const char *path) {
    for (int i = 0; i < g_n_import_paths; i++)
        if (strcmp(g_import_paths[i], path) == 0)
            return;
    if (g_n_import_paths < MAX_IMPORT_PATHS)
        snprintf(g_import_paths[g_n_import_paths++], sizeof g_import_paths[0], "%s", path);
}

static void dump_json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"') fputs("\\\"", f);
        else if (c == '\\') fputs("\\\\", f);
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\r') fputs("\\r", f);
        else if (c == '\t') fputs("\\t", f);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

/* Записывает сообщения из памяти во временный JSONL; возвращает число строк (0 - нечего писать/не удалось). */
static int dump_rows_to_temp(char *out_path, size_t cap) {
    WCHAR tmpdir[MAX_PATH], tmpfile[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmpdir) || !GetTempFileNameW(tmpdir, L"acr", 0, tmpfile))
        return 0;
    WideCharToMultiByte(CP_UTF8, 0, tmpfile, -1, out_path, (int)cap, NULL, NULL);
    FILE *f = _wfopen(tmpfile, L"wb");
    if (!f)
        return 0;
    int n = 0;
    for (int i = 0; i < g_all_rows_count; i++) {
        const TableRow *r = &g_all_rows[i];
        char t[64], src[320], md[8], ad[460], lb[16], ld[640], tx[1200];
        wide_to_utf8(r->time, t, sizeof t);
        wide_to_utf8(r->source, src, sizeof src);
        wide_to_utf8(r->mode, md, sizeof md);
        wide_to_utf8(r->addr, ad, sizeof ad);
        wide_to_utf8(r->label, lb, sizeof lb);
        wide_to_utf8(r->label_desc, ld, sizeof ld);
        wide_to_utf8(r->text, tx, sizeof tx);
        char *dash = strstr(ad, " \xe2\x80\x94 "); /* "RA73104 — Airbus A320 ..." -> только регистрация */
        if (dash) *dash = '\0';
        if (strcmp(ad, "(?)") == 0) ad[0] = '\0';
        fputs("{\"timestamp\":", f); dump_json_str(f, t);
        fputs(",\"source\":", f);    dump_json_str(f, src);
        fputs(",\"crc_status\":\"OK\",\"mode\":", f); dump_json_str(f, md);
        fputs(",\"addr\":", f);      dump_json_str(f, ad);
        fputs(",\"label\":", f);     dump_json_str(f, lb);
        fputs(",\"label_description\":", f); dump_json_str(f, ld);
        fputs(",\"text\":", f);      dump_json_str(f, tx);
        fputs("}\n", f);
        n++;
    }
    fclose(f);
    return n;
}

/* Собирает список источников; tmp_out - путь временного файла (его нужно удалить после выгрузки) или "". */
static int build_export_inputs(const char **paths, int max, char *tmp_out, size_t tmp_cap) {
    int n = 0;
    paths[n++] = "acars_logs\\acars_log.jsonl";
    paths[n++] = "acars_log.jsonl";
    for (int i = 0; i < g_n_import_paths && n < max - 1; i++)
        paths[n++] = g_import_paths[i];
    tmp_out[0] = '\0';
    if (dump_rows_to_temp(tmp_out, tmp_cap) > 0 && n < max)
        paths[n++] = tmp_out;
    return n;
}

static void drop_temp_inputs(const char *tmp) {
    if (tmp && tmp[0]) {
        WCHAR w[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, tmp, -1, w, MAX_PATH);
        DeleteFileW(w);
    }
}

/* ---------- Выгрузка журнала с заданного времени: JSONL или Excel ---------- */

#define IDC_EXP_FROM  301
#define IDC_EXP_TO    302
#define IDC_EXP_XLSX  303
#define IDC_EXP_JSONL 304
#define IDC_EXP_SKIP  305
#define IDC_EXP_REG   306

typedef struct {
    char from[20], to[20]; /* "" - не ограничено */
    int xlsx;
    int skip_service;
    char reg[40]; /* "" - все борта */
    int ok;
} ExportDlgData;

static ExportDlgData g_exp;
static int g_exp_done;

static void exp_read_edit(HWND dlg, int id, char *out, size_t cap) {
    WCHAR w[96];
    w[0] = L'\0';
    GetDlgItemTextW(dlg, id, w, 96);
    wide_to_utf8(w, out, cap);
}

static LRESULT CALLBACK ExportDlgProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_EXP_XLSX || id == IDC_EXP_JSONL) {
            /* для Excel служебные подтверждения обычно не нужны, для JSONL - полная выгрузка */
            CheckDlgButton(h, IDC_EXP_SKIP, id == IDC_EXP_XLSX ? BST_CHECKED : BST_UNCHECKED);
            return 0;
        }
        if (id == IDOK) {
            char a[96], b[96];
            exp_read_edit(h, IDC_EXP_FROM, a, sizeof(a));
            exp_read_edit(h, IDC_EXP_TO, b, sizeof(b));
            g_exp.from[0] = g_exp.to[0] = '\0';
            if (a[0] && !journal_parse_time(a, 0, g_exp.from)) {
                MessageBoxW(h, L"Не понял время «С». Примеры: 06.10.2026 14:30,  2026-10-06 14:30,  06.10.2026,  14:30 (сегодня).",
                            L"Выгрузка", MB_OK | MB_ICONWARNING);
                return 0;
            }
            if (b[0] && !journal_parse_time(b, 1, g_exp.to)) {
                MessageBoxW(h, L"Не понял время «По». Примеры: 06.10.2026 18:00,  06.10.2026,  18:00 (сегодня). Можно оставить пустым - до конца журнала.",
                            L"Выгрузка", MB_OK | MB_ICONWARNING);
                return 0;
            }
            if (g_exp.from[0] && g_exp.to[0] && strcmp(g_exp.from, g_exp.to) > 0) {
                MessageBoxW(h, L"Время «С» позже времени «По».", L"Выгрузка", MB_OK | MB_ICONWARNING);
                return 0;
            }
            exp_read_edit(h, IDC_EXP_REG, g_exp.reg, sizeof(g_exp.reg));
            g_exp.xlsx = IsDlgButtonChecked(h, IDC_EXP_XLSX) == BST_CHECKED;
            g_exp.skip_service = IsDlgButtonChecked(h, IDC_EXP_SKIP) == BST_CHECKED;
            g_exp.ok = 1;
            g_exp_done = 1;
            return 0;
        }
        if (id == IDCANCEL) {
            g_exp_done = 1;
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        g_exp_done = 1;
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static BOOL save_dialog_keep_cwd(OPENFILENAMEW *ofn) {
    WCHAR saved[MAX_PATH];
    saved[0] = L'\0';
    GetCurrentDirectoryW(MAX_PATH, saved);
    ofn->Flags |= OFN_NOCHANGEDIR;
    BOOL r = GetSaveFileNameW(ofn);
    if (saved[0])
        SetCurrentDirectoryW(saved);
    return r;
}

static void export_dialog_register(void) {
    static int registered;
    if (!registered) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = ExportDlgProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"AcarsExportDlgClass";
        RegisterClassW(&wc);
        registered = 1;
    }
}

static void export_dialog(HWND owner) {
    export_dialog_register();

    /* значение "С" по умолчанию - сегодня 00:00 */
    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR def_from[48];
    wsprintfW(def_from, L"%02d.%02d.%04d 00:00", st.wDay, st.wMonth, st.wYear);

    RECT orc;
    GetWindowRect(owner, &orc);
    int W = 460, H = 372;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"AcarsExportDlgClass", L"Выгрузка журнала",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, orc.left + 60, orc.top + 60, W, H, owner,
                               NULL, GetModuleHandleW(NULL), NULL);
    if (!dlg)
        return;
    HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
#define EXP_CTL(hw) SendMessageW((hw), WM_SETFONT, (WPARAM)f, TRUE)
    HWND c;
    c = CreateWindowW(L"STATIC", L"Выгрузить сообщения за период: журнал программы, загруженные JSONL и всё, что сейчас в памяти (время - местное, как в журнале):",
                      WS_CHILD | WS_VISIBLE, 14, 12, 425, 34, dlg, NULL, NULL, NULL);
    EXP_CTL(c);
    c = CreateWindowW(L"STATIC", L"С:", WS_CHILD | WS_VISIBLE, 14, 56, 28, 20, dlg, NULL, NULL, NULL);
    EXP_CTL(c);
    c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", def_from, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                        44, 53, 160, 24, dlg, (HMENU)IDC_EXP_FROM, NULL, NULL);
    EXP_CTL(c);
    c = CreateWindowW(L"STATIC", L"По:", WS_CHILD | WS_VISIBLE, 224, 56, 28, 20, dlg, NULL, NULL, NULL);
    EXP_CTL(c);
    c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                        256, 53, 160, 24, dlg, (HMENU)IDC_EXP_TO, NULL, NULL);
    EXP_CTL(c);
    c = CreateWindowW(L"STATIC",
                      L"Форматы: 06.10.2026 14:30  |  2026-10-06 14:30  |  06.10.2026  |  14:30 (сегодня). "
                      L"Пустое «По» - до конца журнала, пустое «С» - с самого начала.",
                      WS_CHILD | WS_VISIBLE, 14, 86, 425, 34, dlg, NULL, NULL, NULL);
    EXP_CTL(c);
    c = CreateWindowW(L"BUTTON", L"Excel (.xlsx) - таблица по колонкам, с расшифровкой и сводкой по бортам",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, 14, 128, 425, 22, dlg,
                      (HMENU)IDC_EXP_XLSX, NULL, NULL);
    EXP_CTL(c);
    c = CreateWindowW(L"BUTTON", L"JSONL - тот же формат, что у журнала (можно загрузить через «Импорт»)",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, 14, 152, 425, 22, dlg,
                      (HMENU)IDC_EXP_JSONL, NULL, NULL);
    EXP_CTL(c);
    CheckDlgButton(dlg, IDC_EXP_XLSX, BST_CHECKED);
    c = CreateWindowW(L"BUTTON", L"Без служебных сообщений (подтверждения «_», проверка канала Q0, SQ)",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 14, 184, 425, 22, dlg,
                      (HMENU)IDC_EXP_SKIP, NULL, NULL);
    EXP_CTL(c);
    CheckDlgButton(dlg, IDC_EXP_SKIP, BST_CHECKED);
    c = CreateWindowW(L"STATIC", L"Только борт:", WS_CHILD | WS_VISIBLE, 14, 220, 90, 20, dlg, NULL, NULL, NULL);
    EXP_CTL(c);
    c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                        108, 217, 130, 24, dlg, (HMENU)IDC_EXP_REG, NULL, NULL);
    EXP_CTL(c);
    c = CreateWindowW(L"STATIC", L"например RA73104 или B-1073; пусто - все борта", WS_CHILD | WS_VISIBLE, 246, 220,
                      200, 36, dlg, NULL, NULL, NULL);
    EXP_CTL(c);
    c = CreateWindowW(L"BUTTON", L"Выгрузить...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                      226, 290, 106, 28, dlg, (HMENU)IDOK, NULL, NULL);
    EXP_CTL(c);
    c = CreateWindowW(L"BUTTON", L"Отмена", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 338, 290, 100,
                      28, dlg, (HMENU)IDCANCEL, NULL, NULL);
    EXP_CTL(c);
#undef EXP_CTL

    memset(&g_exp, 0, sizeof(g_exp));
    g_exp_done = 0;
    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(GetDlgItem(dlg, IDC_EXP_FROM));
    MSG msg;
    while (!g_exp_done && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    DestroyWindow(dlg);
    SetForegroundWindow(owner);
    if (!g_exp.ok)
        return;

    /* куда сохранять */
    WCHAR path[MAX_PATH];
    wsprintfW(path, L"acars_export_%04d%02d%02d_%02d%02d.%s", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
              g_exp.xlsx ? L"xlsx" : L"jsonl");
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = g_exp.xlsx ? L"Excel (*.xlsx)\0*.xlsx\0" : L"Журнал ACARS (*.jsonl)\0*.jsonl\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = g_exp.xlsx ? L"xlsx" : L"jsonl";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrTitle = L"Куда сохранить выгрузку";
    if (!save_dialog_keep_cwd(&ofn))
        return;

    char out_utf8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, out_utf8, sizeof(out_utf8), NULL, NULL);
    /* журналы - те же, что читает автозагрузка (рабочая папка программы) */
    const char *in_paths[16];
    char tmp_in[MAX_PATH * 3];
    int n_in = build_export_inputs(in_paths, 16, tmp_in, sizeof tmp_in);
    ExportFilter flt;
    flt.from = g_exp.from[0] ? g_exp.from : NULL;
    flt.to = g_exp.to[0] ? g_exp.to : NULL;
    flt.skip_service = g_exp.skip_service;
    flt.reg = g_exp.reg[0] ? g_exp.reg : NULL;

    ExportStats es;
    HCURSOR old = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
    int r = g_exp.xlsx ? journal_export_xlsx(in_paths, n_in, out_utf8, &flt, &es)
                       : journal_export_jsonl(in_paths, n_in, out_utf8, &flt, &es);
    SetCursor(old);
    drop_temp_inputs(tmp_in);

    WCHAR msg2[1400];
    if (r == -1) {
        MessageBoxW(owner, L"Журнал не найден: нет ни acars_logs\\acars_log.jsonl, ни acars_log.jsonl в рабочей папке программы.",
                    L"Выгрузка", MB_OK | MB_ICONWARNING);
    } else if (r != 0) {
        MessageBoxW(owner, L"Не удалось создать файл. Проверьте, что он не открыт в Excel и что в папку можно писать.",
                    L"Выгрузка", MB_OK | MB_ICONERROR);
    } else if (es.written == 0) {
        DeleteFileW(path);
        wsprintfW(msg2, L"За выбранный период сообщений не нашлось (в журнале прочитано строк: %d). Файл не создан.", es.read);
        MessageBoxW(owner, msg2, L"Выгрузка", MB_OK | MB_ICONINFORMATION);
    } else {
        if (g_exp.xlsx)
            wsprintfW(msg2, L"Готово: выгружено сообщений %d (на листе «Борта»: %d бортов).\r\nПропущено вне периода/служебных: %d, повторов: %d.\r\n\r\nФайл:\r\n%s",
                      es.written, es.aircraft, es.skipped, es.dups, path);
        else
            wsprintfW(msg2, L"Готово: выгружено сообщений %d.\r\nПропущено вне периода/служебных: %d, повторов: %d.\r\n\r\nФайл:\r\n%s",
                      es.written, es.skipped, es.dups, path);
        MessageBoxW(owner, msg2, L"Выгрузка", MB_OK | MB_ICONINFORMATION);
    }
}


/* ---------- Все сообщения одного борта за период ---------- */

static void aircraft_report_dialog(HWND owner, const char *reg) {
    /* окно использует тот же класс и обработчик, что и выгрузка (поля С/По/Борт, кнопки OK/Отмена) */
    export_dialog_register();
    WCHAR wreg[48];
    MultiByteToWideChar(CP_UTF8, 0, reg, -1, wreg, 48);

    RECT orc;
    GetWindowRect(owner, &orc);
    int W = 460, H = 250;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"AcarsExportDlgClass", L"Все сообщения борта",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, orc.left + 80, orc.top + 80, W, H, owner, NULL,
                               GetModuleHandleW(NULL), NULL);
    if (!dlg)
        return;
    HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
#define EXP_CTL(hw) SendMessageW((hw), WM_SETFONT, (WPARAM)f, TRUE)
    HWND c;
    c = CreateWindowW(L"STATIC", L"Показать все сообщения борта из журналов (за все дни, что есть в журнале). Время - местное, как в журнале.",
                      WS_CHILD | WS_VISIBLE, 14, 10, 425, 34, dlg, NULL, NULL, NULL);
    EXP_CTL(c);
    c = CreateWindowW(L"STATIC", L"Борт:", WS_CHILD | WS_VISIBLE, 14, 54, 40, 20, dlg, NULL, NULL, NULL);
    EXP_CTL(c);
    c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", wreg, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 58, 51,
                        150, 24, dlg, (HMENU)IDC_EXP_REG, NULL, NULL);
    EXP_CTL(c);
    c = CreateWindowW(L"STATIC", L"С:", WS_CHILD | WS_VISIBLE, 14, 90, 28, 20, dlg, NULL, NULL, NULL);
    EXP_CTL(c);
    c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 44, 87,
                        160, 24, dlg, (HMENU)IDC_EXP_FROM, NULL, NULL);
    EXP_CTL(c);
    c = CreateWindowW(L"STATIC", L"По:", WS_CHILD | WS_VISIBLE, 224, 90, 28, 20, dlg, NULL, NULL, NULL);
    EXP_CTL(c);
    c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 256, 87,
                        160, 24, dlg, (HMENU)IDC_EXP_TO, NULL, NULL);
    EXP_CTL(c);
    c = CreateWindowW(L"STATIC",
                      L"Пусто - все дни. Примеры: 06.10.2026 | 06.10.2026 14:30 | 14:30 (сегодня). Можно выбрать часы одного дня или несколько дней.",
                      WS_CHILD | WS_VISIBLE, 14, 120, 425, 36, dlg, NULL, NULL, NULL);
    EXP_CTL(c);
    c = CreateWindowW(L"BUTTON", L"Показать", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 226, 168, 106,
                      28, dlg, (HMENU)IDOK, NULL, NULL);
    EXP_CTL(c);
    c = CreateWindowW(L"BUTTON", L"Отмена", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 338, 168, 100, 28,
                      dlg, (HMENU)IDCANCEL, NULL, NULL);
    EXP_CTL(c);
#undef EXP_CTL

    memset(&g_exp, 0, sizeof(g_exp));
    g_exp_done = 0;
    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(GetDlgItem(dlg, IDC_EXP_FROM));
    MSG msg;
    while (!g_exp_done && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    DestroyWindow(dlg);
    SetForegroundWindow(owner);
    if (!g_exp.ok)
        return;
    if (!g_exp.reg[0]) {
        MessageBoxW(owner, L"Не указан борт.", L"Сообщения борта", MB_OK | MB_ICONWARNING);
        return;
    }

    const char *in_paths[16];
    char tmp_in[MAX_PATH * 3];
    int n_in = build_export_inputs(in_paths, 16, tmp_in, sizeof tmp_in);
    ExportFilter flt;
    memset(&flt, 0, sizeof(flt));
    flt.from = g_exp.from[0] ? g_exp.from : NULL;
    flt.to = g_exp.to[0] ? g_exp.to : NULL;
    ExportStats es;
    HCURSOR old = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
    char *rep = journal_aircraft_report(in_paths, n_in, g_exp.reg, &flt, &es);
    SetCursor(old);
    drop_temp_inputs(tmp_in);
    if (!rep) {
        MessageBoxW(owner, L"Журнал не найден: нет ни acars_logs\\acars_log.jsonl, ни acars_log.jsonl в рабочей папке программы.",
                    L"Сообщения борта", MB_OK | MB_ICONWARNING);
        return;
    }
    if (es.written == 0) {
        WCHAR m2[300];
        wsprintfW(m2, L"Сообщений борта %s за выбранный период в журнале нет (просмотрено строк: %d).", wreg, es.read);
        MessageBoxW(owner, m2, L"Сообщения борта", MB_OK | MB_ICONINFORMATION);
        free(rep);
        return;
    }
    /* перевод строки "\n" -> "\r\n" для текстового окна */
    size_t nl = 0;
    for (const char *p = rep; *p; p++)
        if (*p == '\n') nl++;
    size_t len = strlen(rep);
    char *crlf = (char *)malloc(len + nl + 1);
    if (crlf) {
        char *q = crlf;
        for (const char *p = rep; *p; p++) {
            if (*p == '\n') *q++ = '\r';
            *q++ = *p;
        }
        *q = '\0';
        WCHAR *w = utf8_to_wide(crlf);
        if (w) {
            WCHAR title[96];
            wsprintfW(title, L"Все сообщения борта %s", wreg);
            show_text_window(owner, title, w);
            free(w);
        }
        free(crlf);
    }
    free(rep);
}

static void import_jsonl_dialog(HWND owner) {
    WCHAR path[MAX_PATH];
    path[0] = L'\0';

    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Журналы ACARS (*.jsonl)\0*.jsonl\0Все файлы (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrTitle = L"Выберите журнал acars_log.jsonl для импорта";

    if (!open_dialog_keep_cwd(&ofn)) {
        DWORD err = CommDlgExtendedError();
        if (err != 0) {
            WCHAR errbuf[256];
            wsprintfW(errbuf, L"Диалог выбора файла завершился с ошибкой (код %lu).",
                      (unsigned long)err);
            MessageBoxW(owner, errbuf, L"ACARS-декодер - ошибка", MB_OK | MB_ICONERROR);
        }
        return;
    }
    import_jsonl_file(owner, path);
}

static void open_file_dialog(HWND owner) {
    WCHAR path[MAX_PATH];
    path[0] = L'\0';

    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"WAV-файлы (*.wav)\0*.wav\0Все файлы (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrTitle = L"Выберите WAV-файл для декодирования";

    if (!open_dialog_keep_cwd(&ofn)) {
        /* CommDlgExtendedError()==0 означает, что пользователь просто
         * нажал "Отмена" - это нормально, молчим. Любой другой код -
         * настоящая ошибка (например, несовпадение размера структуры
         * OPENFILENAMEW), и раньше здесь тоже был тихий отказ без
         * единого сообщения - показываем код, чтобы было видно, что
         * именно пошло не так. */
        DWORD err = CommDlgExtendedError();
        if (err != 0) {
            WCHAR errbuf[256];
            wsprintfW(errbuf, L"Диалог выбора файла завершился с ошибкой (код %lu).",
                      (unsigned long)err);
            MessageBoxW(owner, errbuf, L"ACARS-декодер - ошибка", MB_OK | MB_ICONERROR);
        }
        return;
    }

    int len = WideCharToMultiByte(CP_UTF8, 0, path, -1, NULL, 0, NULL, NULL);
    char *utf8_path = (char *)malloc((size_t)len);
    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8_path, len, NULL, NULL);

    g_file_decoding = 1;
    EnableWindow(g_hBtnFile, FALSE);
    set_status(L"Декодирую файл...", CLR_FG_TEXT);
    CreateThread(NULL, 0, file_decode_thread_proc, utf8_path, 0, NULL);
}

/* ---------- главное окно ---------- */

#define ID_AUTOSTART_TIMER_AR       1
#define ID_AUTOSTART_TIMER_CAPTURE  2

/* Реализация - см. перед run_gui() ниже по файлу (там же, где
 * вызываются Get/WritePrivateProfileStringW) - опережающее объявление
 * нужно, поскольку сами функции используются уже здесь, в WndProc. */
static void save_settings(HWND hwnd);
static void load_settings(HWND hwnd);

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hCombo = CreateWindowW(L"COMBOBOX", NULL,
                                  WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                  10, 56, 500, 200, hwnd, (HMENU)IDC_COMBO_DEVICE,
                                  NULL, NULL);
        g_hBtnRefresh = CreateWindowW(L"BUTTON", L"Обновить",
                                       WS_CHILD | WS_VISIBLE | BS_FLAT, 520, 56, 100, 24,
                                       hwnd, (HMENU)IDC_BTN_REFRESH, NULL, NULL);
        WCHAR start_txt[48];
        wsprintfW(start_txt, L"%s Начать приём", xp_play_glyph());
        g_hBtnStart = CreateWindowW(L"BUTTON", start_txt,
                                     WS_CHILD | WS_VISIBLE | BS_FLAT, 10, 90, 200, 28,
                                     hwnd, (HMENU)IDC_BTN_START, NULL, NULL);
        g_hBtnFile = CreateWindowW(L"BUTTON", L"Открыть WAV-файл...",
                                    WS_CHILD | WS_VISIBLE | BS_FLAT, 220, 90, 200, 28,
                                    hwnd, (HMENU)IDC_BTN_FILE, NULL, NULL);
        g_hBtnClear = CreateWindowW(L"BUTTON", L"Очистить лог",
                                     WS_CHILD | WS_VISIBLE | BS_FLAT, 430, 90, 130, 28,
                                     hwnd, (HMENU)IDC_BTN_CLEAR, NULL, NULL);
        g_hBtnView = CreateWindowW(L"BUTTON", L"\u25A4 Таблица",
                                    WS_CHILD | WS_VISIBLE | BS_FLAT, 570, 90, 110, 28,
                                    hwnd, (HMENU)IDC_BTN_VIEW, NULL, NULL);
        g_hBtnStats = CreateWindowW(L"BUTTON", L"Статистика",
                                     WS_CHILD | WS_VISIBLE | BS_FLAT, 690, 90, 120, 28,
                                     hwnd, (HMENU)IDC_BTN_STATS, NULL, NULL);
        g_hBtnRoutes = CreateWindowW(L"BUTTON", L"Маршруты",
                                      WS_CHILD | WS_VISIBLE | BS_FLAT, 820, 90, 120, 28,
                                      hwnd, (HMENU)IDC_BTN_ROUTES, NULL, NULL);
        g_hBtnImport = CreateWindowW(L"BUTTON", L"Импорт JSONL...",
                                      WS_CHILD | WS_VISIBLE | BS_FLAT, 950, 90, 130, 28,
                                      hwnd, (HMENU)IDC_BTN_IMPORT, NULL, NULL);
        g_hBtnExport = CreateWindowW(L"BUTTON", L"Выгрузка...",
                                      WS_CHILD | WS_VISIBLE | BS_FLAT, 1090, 90, 130, 28,
                                      hwnd, (HMENU)IDC_BTN_EXPORT, NULL, NULL);
        g_hCheckTopmost = CreateWindowW(L"BUTTON", L"Поверх всех окон",
                                         WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                         10, 128, 160, 20, hwnd, (HMENU)IDC_CHECK_TOPMOST,
                                         NULL, NULL);
        g_hBtnFaults = CreateWindowW(L"BUTTON", L"Неисправности",
                                      WS_CHILD | WS_VISIBLE | BS_FLAT, 190, 124, 140, 24,
                                      hwnd, (HMENU)IDC_BTN_FAULTS, NULL, NULL);
        g_hBtnTelemetry = CreateWindowW(L"BUTTON", L"Телеметрия",
                                         WS_CHILD | WS_VISIBLE | BS_FLAT, 340, 124, 120, 24,
                                         hwnd, (HMENU)IDC_BTN_TELEMETRY, NULL, NULL);
        g_hBtnWeather = CreateWindowW(L"BUTTON", L"Погода",
                                       WS_CHILD | WS_VISIBLE | BS_FLAT, 470, 124, 110, 24,
                                       hwnd, (HMENU)IDC_BTN_WEATHER, NULL, NULL);
        g_hBtnDetails = CreateWindowW(L"BUTTON", L"Подробности",
                                       WS_CHILD | WS_VISIBLE | BS_FLAT, 590, 124, 140, 24,
                                       hwnd, (HMENU)IDC_BTN_DETAILS, NULL, NULL);
        g_hBtnMap = CreateWindowW(L"BUTTON", L"Карта",
                                   WS_CHILD | WS_VISIBLE | BS_FLAT, 740, 124, 100, 24,
                                   hwnd, (HMENU)IDC_BTN_MAP, NULL, NULL);
        g_hCheckAutostart = CreateWindowW(L"BUTTON", L"Автостарт приёма при запуске",
                                           WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                           850, 128, 220, 20, hwnd, (HMENU)IDC_CHECK_AUTOSTART,
                                           NULL, NULL);

        /* --- Панель управления AR5000 по RS-232 ---
         * Каждая логическая группа - на своей отдельной строке, с
         * запасом по высоте (24-28px) и промежутком между строками -
         * раньше несколько групп делили одну и ту же строку, из-за
         * чего при добавлении новых элементов легко было забыть
         * сдвинуть что-то ещё и получить наложение (что и
         * произошло). Явная "одна группа - одна строка" схема легче
         * читается и меньше подвержена такой ошибке при дальнейших
         * правках. */
        CreateWindowW(L"STATIC", L"COM-порт:", WS_CHILD | WS_VISIBLE,
                      10, 177, 80, 20, hwnd, NULL, NULL, NULL);
        g_hEditCom = CreateWindowW(L"EDIT", L"COM3", WS_CHILD | WS_VISIBLE | WS_BORDER,
                                    95, 174, 60, 22, hwnd, (HMENU)IDC_EDIT_COM, NULL, NULL);
        CreateWindowW(L"STATIC", L"Скорость:", WS_CHILD | WS_VISIBLE,
                      170, 177, 80, 20, hwnd, NULL, NULL, NULL);
        g_hComboBaud = CreateWindowW(L"COMBOBOX", NULL,
                                      WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                      255, 174, 70, 100, hwnd, (HMENU)IDC_COMBO_BAUD, NULL, NULL);
        g_hBtnArConnect = CreateWindowW(L"BUTTON", L"Подключить", WS_CHILD | WS_VISIBLE | BS_FLAT,
                                         340, 174, 110, 24, hwnd, (HMENU)IDC_BTN_AR_CONNECT, NULL, NULL);

        CreateWindowW(L"STATIC", L"Частота, МГц:", WS_CHILD | WS_VISIBLE,
                      10, 211, 130, 20, hwnd, NULL, NULL, NULL);
        g_hEditFreq = CreateWindowW(L"EDIT", L"131.725", WS_CHILD | WS_VISIBLE | WS_BORDER,
                                     145, 208, 80, 22, hwnd, (HMENU)IDC_EDIT_FREQ, NULL, NULL);
        g_hBtnTune = CreateWindowW(L"BUTTON", L"Настроить", WS_CHILD | WS_VISIBLE | BS_FLAT,
                                    235, 208, 100, 24, hwnd, (HMENU)IDC_BTN_TUNE, NULL, NULL);
        g_hComboPreset = CreateWindowW(L"COMBOBOX", NULL,
                                        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                        345, 208, 90, 150, hwnd, (HMENU)IDC_COMBO_PRESET, NULL, NULL);

        CreateWindowW(L"STATIC", L"Режим:", WS_CHILD | WS_VISIBLE,
                      10, 245, 65, 20, hwnd, NULL, NULL, NULL);
        g_hComboMode = CreateWindowW(L"COMBOBOX", NULL,
                                      WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                      80, 242, 100, 150, hwnd, (HMENU)IDC_COMBO_MODE, NULL, NULL);
        CreateWindowW(L"STATIC", L"Полоса:", WS_CHILD | WS_VISIBLE,
                      195, 245, 70, 20, hwnd, NULL, NULL, NULL);
        g_hComboBW = CreateWindowW(L"COMBOBOX", NULL,
                                    WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                    270, 242, 100, 150, hwnd, (HMENU)IDC_COMBO_BW, NULL, NULL);

        CreateWindowW(L"STATIC", L"Банк памяти:", WS_CHILD | WS_VISIBLE,
                      10, 279, 115, 20, hwnd, NULL, NULL, NULL);
        g_hComboMemBank = CreateWindowW(L"COMBOBOX", NULL,
                                         WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                         130, 276, 60, 150, hwnd, (HMENU)IDC_COMBO_MEMBANK, NULL, NULL);
        g_hBtnMemList = CreateWindowW(L"BUTTON", L"Банки памяти...",
                                       WS_CHILD | WS_VISIBLE | BS_FLAT,
                                       200, 276, 140, 24, hwnd, (HMENU)IDC_BTN_MEMLIST, NULL, NULL);

        g_hBtnScan = CreateWindowW(L"BUTTON", L"\u21BB Сканировать",
                                    WS_CHILD | WS_VISIBLE | BS_FLAT,
                                    10, 313, 160, 24, hwnd, (HMENU)IDC_BTN_SCAN, NULL, NULL);
        g_hArProgress = CreateWindowW(L"msctls_progress32", NULL,
                                       WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                       180, 316, 400, 18, hwnd, NULL, NULL, NULL);
        if (g_hArProgress) {
            SendMessageW(g_hArProgress, PBM_SETRANGE32, 0, 255);
            SendMessageW(g_hArProgress, PBM_SETPOS, 0, 0);
            /* Тёмный фон + фосфорно-зелёная полоса - в тон общей теме
             * (PBM_SETBARCOLOR/PBM_SETBKCOLOR поддерживаются с Common
             * Controls 5.80 - т.е. везде, где у нас вообще появляется
             * ICC_PROGRESS_CLASS). */
            SendMessageW(g_hArProgress, PBM_SETBKCOLOR, 0, (LPARAM)CLR_BG_LOG);
            SendMessageW(g_hArProgress, PBM_SETBARCOLOR, 0, (LPARAM)CLR_FG_OK);
        }
        g_hBtnDiag = CreateWindowW(L"BUTTON", L"Диагностика",
                                    WS_CHILD | WS_VISIBLE | BS_FLAT, 595, 313, 120, 24,
                                    hwnd, (HMENU)IDC_BTN_DIAG, NULL, NULL);
        g_hCheckNotify = CreateWindowW(L"BUTTON", L"Уведомления",
                                        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                        730, 317, 140, 20, hwnd, (HMENU)IDC_CHECK_NOTIFY,
                                        NULL, NULL);
        /* Уведомления ВЫКЛЮЧЕНЫ по умолчанию при первом запуске - кто
         * захочет, включит сам; значение из acars_settings.ini (если
         * файл уже есть) всё равно переопределит это чуть ниже, в
         * load_settings(). */

        g_hArStatus = CreateWindowW(L"STATIC", L"\u25CF AR5000: не подключен",
                                     WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS,
                                     10, 347, 940, 20, hwnd, (HMENU)IDC_STATIC_AR_STATUS, NULL, NULL);

        g_hStatus = CreateWindowW(L"STATIC", L"\u25CF Готово.",
                                   WS_CHILD | WS_VISIBLE, 10, 383, 350, 20,
                                   hwnd, (HMENU)IDC_STATIC_STATUS, NULL, NULL);
        CreateWindowW(L"STATIC", L"Поиск:", WS_CHILD | WS_VISIBLE,
                      375, 383, 60, 20, hwnd, NULL, NULL, NULL);
        g_hEditFilter = CreateWindowW(L"EDIT", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER,
                                       440, 380, 300, 22, hwnd, (HMENU)IDC_EDIT_FILTER, NULL, NULL);
        /* EM_SETCUEBANNER - серая подсказка внутри пустого поля,
         * исчезающая при вводе - доступна с Windows XP. Если по
         * какой-то причине не сработает - поле всё равно полностью
         * рабочее, просто без подсказки. */
        SendMessageW(g_hEditFilter, EM_SETCUEBANNER, TRUE,
                     (LPARAM)L"Поиск по таблице: борт, метка, текст...");
        CreateWindowW(L"STATIC", L"Борт:", WS_CHILD | WS_VISIBLE, 752, 383, 44, 20, hwnd, NULL, NULL, NULL);
        g_hEditAircraft = CreateWindowW(L"EDIT", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER, 798, 380, 130, 22,
                                        hwnd, (HMENU)IDC_EDIT_AIRCRAFT, NULL, NULL);
        SendMessageW(g_hEditAircraft, EM_SETCUEBANNER, TRUE, (LPARAM)L"RA73704");

        /* RichEdit вместо обычного EDIT - нужен, чтобы раскрашивать
         * строки лога по смыслу (см. pick_line_color): обычный EDIT
         * умеет только один цвет текста на весь control. riched20.dll
         * (класс окна "RichEdit20W") входит в поставку Windows начиная
         * с 98/2000, так что доступен на всех версиях, которые мы
         * поддерживаем (XP и новее). Если библиотека по какой-то
         * причине не загрузилась - откатываемся на обычный EDIT
         * (лог будет работать, просто без цветной подсветки). */
        HMODULE hRichEdit = LoadLibraryW(L"riched20.dll");
        LPCWSTR logClass = hRichEdit ? L"RichEdit20W" : L"EDIT";

        g_hEditLog = CreateWindowW(logClass, NULL,
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_BORDER |
                                        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                    10, 417, 600, 275, hwnd, (HMENU)IDC_EDIT_LOG,
                                    NULL, NULL);
        if (hRichEdit) {
            SendMessageW(g_hEditLog, EM_SETBKGNDCOLOR, 0, (LPARAM)CLR_BG_LOG);
            SendMessageW(g_hEditLog, EM_EXLIMITTEXT, 0, (LPARAM)4000000);
        }

        /* Табличный вид (переключается кнопкой "Таблица"/"Лог", делит
         * то же место в окне с g_hEditLog - виден только один из двух
         * одновременно). Колонки сортируются по клику на заголовок
         * (см. WM_NOTIFY/LVN_COLUMNCLICK ниже). */
        g_hTable = CreateWindowW(L"SysListView32", NULL,
                                  WS_CHILD | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS,
                                  10, 417, 600, 275, hwnd, (HMENU)IDC_TABLE, NULL, NULL);
        if (g_hTable) {
            ListView_SetExtendedListViewStyle(g_hTable, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            LVCOLUMNW col;
            memset(&col, 0, sizeof(col));
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            const WCHAR *headers[] = {L"Время", L"Источник", L"Режим", L"Борт",
                                       L"Метка", L"Расшифровка метки", L"Текст"};
            int widths[] = {135, 150, 55, 220, 55, 230, 300};
            for (int i = 0; i < 7; i++) {
                col.iSubItem = i;
                col.pszText = (LPWSTR)headers[i];
                col.cx = widths[i];
                ListView_InsertColumn(g_hTable, i, &col);
            }
        }

        HFONT hfont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        SendMessageW(g_hCombo, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hBtnRefresh, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hBtnStart, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hBtnFile, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hBtnClear, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hBtnView, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hTable, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hEditCom, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hComboBaud, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hBtnArConnect, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hEditFreq, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hBtnTune, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hComboPreset, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hComboMode, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hComboBW, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hComboMemBank, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hBtnMemList, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hArStatus, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hEditFilter, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hEditAircraft, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hBtnScan, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hBtnStats, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hBtnRoutes, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hBtnImport, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hBtnExport, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hBtnFaults, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hBtnTelemetry, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hBtnWeather, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hBtnDetails, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hBtnMap, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hCheckAutostart, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hBtnDiag, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hCheckNotify, WM_SETFONT, (WPARAM)hfont, TRUE);
        SendMessageW(g_hCheckTopmost, WM_SETFONT, (WPARAM)hfont, TRUE);
        HFONT hStatusFont = CreateFontW(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                         RUSSIAN_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         DEFAULT_QUALITY, DEFAULT_PITCH, xp_ui_face());
        SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)hStatusFont, TRUE);
        /* Моноширинный шрифт для лога - расшифрованные сообщения
         * выровнены табличкой (Mode/Addr/Label/...), с ним читается
         * заметно удобнее, чем с обычным пропорциональным. */
        HFONT hmono = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   RUSSIAN_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   DEFAULT_QUALITY, FIXED_PITCH, L"Consolas");
        SendMessageW(g_hEditLog, WM_SETFONT, (WPARAM)hmono, TRUE);

        /* Заполняем список скоростей (должна совпадать с CONFIG-меню
         * самого приёмника) и список частых частот ACARS. */
        SendMessageW(g_hComboBaud, CB_ADDSTRING, 0, (LPARAM)L"4800");
        SendMessageW(g_hComboBaud, CB_ADDSTRING, 0, (LPARAM)L"9600");
        SendMessageW(g_hComboBaud, CB_ADDSTRING, 0, (LPARAM)L"19200");
        SendMessageW(g_hComboBaud, CB_SETCURSEL, 1, 0); /* 9600 по умолчанию */

        SendMessageW(g_hComboPreset, CB_ADDSTRING, 0, (LPARAM)L"Пресеты...");
        for (int i = 0; i < N_ACARS_PRESETS; i++) {
            WCHAR buf[16];
            format_mhz(buf, 16, kAcarsPresets[i]);
            SendMessageW(g_hComboPreset, CB_ADDSTRING, 0, (LPARAM)buf);
        }
        SendMessageW(g_hComboPreset, CB_SETCURSEL, 0, 0);

        /* Вид модуляции (команда MD приёмника) и полоса ПЧ (команда BW) -
         * порядок пунктов в точности соответствует числовым значениям,
         * которые понимает AR5000 (см. ar5000_set_mode/ar5000_set_bandwidth
         * в ar5000.h) - индекс выбора в списке НАПРЯМУЮ передаётся как
         * параметр команды, поэтому порядок менять нельзя. AM/6кГц
         * выбраны по умолчанию - то, что нужно для приёма ACARS. */
        static const WCHAR *kModeNames[] = {L"FM", L"AM", L"LSB", L"USB",
                                             L"CW", L"Sync AM", L"SAL", L"SAH"};
        for (int i = 0; i < 8; i++)
            SendMessageW(g_hComboMode, CB_ADDSTRING, 0, (LPARAM)kModeNames[i]);
        SendMessageW(g_hComboMode, CB_SETCURSEL, 1, 0); /* AM по умолчанию */

        static const WCHAR *kBwNames[] = {L"0.5 кГц", L"3 кГц", L"6 кГц", L"15 кГц",
                                           L"30 кГц", L"110 кГц", L"220 кГц"};
        for (int i = 0; i < 7; i++)
            SendMessageW(g_hComboBW, CB_ADDSTRING, 0, (LPARAM)kBwNames[i]);
        SendMessageW(g_hComboBW, CB_SETCURSEL, 2, 0); /* 6 кГц по умолчанию */

        for (int i = 0; i < 10; i++) {
            WCHAR bnum[4];
            wsprintfW(bnum, L"%d", i);
            SendMessageW(g_hComboMemBank, CB_ADDSTRING, 0, (LPARAM)bnum);
        }
        SendMessageW(g_hComboMemBank, CB_SETCURSEL, 0, 0);

        g_hwnd_main = hwnd;
        msg_sink_register(gui_on_message);
        refresh_device_list();
        load_settings(hwnd); /* после refresh_device_list - иначе восстанавливать выбор
                               * устройства было бы не из чего (список ещё пуст) */
        PostMessageW(hwnd, WM_APP_AUTOLOAD, 0, 0); /* журнал подгрузится сразу после создания окна */
        append_log(L"      ______________________________\r\n"
                    L"     |                              |\r\n"
                    L"     |     \u2708  A C A R S   R A D I O    |\r\n"
                    L"     |______________________________|\r\n"
                    L"            \\              /\r\n"
                    L"     --@--@--\\____________/--@--@--\r\n\r\n");
        append_log(L"Готов к работе. Выберите устройство записи и нажмите \"Начать приём\",\r\n"
                    L"либо откройте WAV-файл для декодирования.\r\n\r\n");

        /* Сообщение о базе бортов - раньше шло через printf()/pipe (как и
         * весь остальной вывод decode_wav_file/run_live_capture), но эта
         * труба - штука асинхронная (отдельный поток-читатель), и хотя
         * теоретически должна работать всегда, на практике оказалась
         * ненадёжной именно для этого самого первого сообщения при
         * старте (возможно, специфика конкретной системы/антивируса,
         * подменяющего поведение потоков). Пишем её так же напрямую и
         * синхронно, как баннер и "Готов к работе" выше - тем самым
         * методом, который точно работает без всяких "может быть". */
        if (aircraft_db_count() > 0) {
            WCHAR buf[256];
            wsprintfW(buf, L"База бортов: загружено %d записей из aircraft_db.csv\r\n\r\n",
                      aircraft_db_count());
            append_log(buf);
        } else if (aircraft_db_last_warning()) {
            WCHAR *w = utf8_to_wide(aircraft_db_last_warning());
            if (w) {
                append_log(w);
                append_log(L"\r\n\r\n");
                free(w);
            }
        }
        if (airport_db_count() > 0) {
            WCHAR buf[256];
            wsprintfW(buf, L"База аэропортов: загружено %d записей из airport_db.csv\r\n\r\n",
                      airport_db_count());
            append_log(buf);
        } else if (airport_db_last_warning()) {
            WCHAR *w = utf8_to_wide(airport_db_last_warning());
            if (w) {
                append_log(w);
                append_log(L"\r\n\r\n");
                free(w);
            }
        }
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        HWND hCtl = (HWND)lParam;
        /* Все статичные подписи в окне - тёмный фон в тон остальному
         * интерфейсу (иначе они рисовались бы светлыми прямоугольниками
         * поверх тёмного окна - системный вид по умолчанию). У g_hStatus
         * и g_hArStatus вдобавок свой, меняющийся по состоянию цвет
         * текста (см. set_status()/WM_APP_AR_STATUS). */
        COLORREF fg = CLR_FG_LABEL;
        if (hCtl == g_hStatus)
            fg = g_status_color;
        else if (hCtl == g_hArStatus)
            fg = g_ar_status_color;
        SetTextColor(hdc, fg);
        SetBkColor(hdc, CLR_BG_MAIN);
        SetBkMode(hdc, OPAQUE);
        return (LRESULT)g_hbrMainBg;
    }

    case WM_CTLCOLORBTN: {
        /* Обычные BUTTON (push-кнопки) сами рисуют свой 3D-бортик и не
         * реагируют на это сообщение - но флажки (BS_AUTOCHECKBOX)
         * рисуют только квадратик+подпись поверх заданного фона, так
         * что для них это работает и убирает светлый прямоугольник на
         * тёмном окне. */
        HWND hCtl = (HWND)lParam;
        if (hCtl == g_hCheckTopmost || hCtl == g_hCheckNotify || hCtl == g_hCheckAutostart) {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, CLR_FG_LABEL);
            SetBkColor(hdc, CLR_BG_MAIN);
            SetBkMode(hdc, OPAQUE);
            return (LRESULT)g_hbrMainBg;
        }
        break;
    }

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_hbrMainBg);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        RECT header = {0, 0, rc.right, 28};
        HBRUSH hHeaderBrush = CreateSolidBrush(RGB(35, 62, 92));
        FillRect(hdc, &header, hHeaderBrush);
        DeleteObject(hHeaderBrush);

        SetTextColor(hdc, RGB(225, 238, 250));
        SetBkMode(hdc, TRANSPARENT);
        HFONT hTitleFont = CreateFontW(-18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                        RUSSIAN_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        DEFAULT_QUALITY, DEFAULT_PITCH, xp_ui_face());
        HFONT hOldFont = (HFONT)SelectObject(hdc, hTitleFont);
        static const WCHAR title[] = L"\u2708 ACARS МОНИТОР \u2014 AOR AR5000";
        TextOutW(hdc, 12, 4, title, (int)wcslen(title));
        SelectObject(hdc, hOldFont);
        DeleteObject(hTitleFont);

        /* Подписи-разделители секций - просто нарисованный текст, не
         * отдельные STATIC-элементы (не нужно их отдельно темизировать,
         * не нужно вручную позиционировать/скрывать при ресайзе - они
         * просто перерисовываются вместе с остальным фоном). */
        HFONT hSectionFont = CreateFontW(-12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                          RUSSIAN_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          DEFAULT_QUALITY, DEFAULT_PITCH, xp_ui_face());
        hOldFont = (HFONT)SelectObject(hdc, hSectionFont);
        SetTextColor(hdc, CLR_FG_DIM);
        static const WCHAR sec1[] = L"\u25B8 ПРИЁМ АУДИО";
        TextOutW(hdc, 12, 36, sec1, (int)wcslen(sec1));
        static const WCHAR sec2[] = L"\u25B8 ПРИЁМНИК AR5000 (RS-232)";
        TextOutW(hdc, 12, 154, sec2, (int)wcslen(sec2));
        SelectObject(hdc, hOldFont);
        DeleteObject(hSectionFont);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        MoveWindow(g_hCombo, 10, 34, w - 130, 200, TRUE);
        MoveWindow(g_hBtnRefresh, w - 110, 34, 100, 24, TRUE);
        MoveWindow(g_hEditLog, 10, 417, w - 20, h - 427, TRUE);
        MoveWindow(g_hTable, 10, 417, w - 20, h - 427, TRUE);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_BTN_REFRESH) {
            refresh_device_list();
        } else if (id == IDC_BTN_CLEAR) {
            if (g_table_view) {
                ListView_DeleteAllItems(g_hTable);
                g_all_rows_count = 0; /* сбрасываем и постоянное хранилище -
                                        * "очистить" должно быть по-настоящему
                                        * полной очисткой, а не только видимой
                                        * части (иначе после следующего же
                                        * изменения фильтра стёртые строки
                                        * "воскресли" бы обратно) */
            } else {
                SetWindowTextW(g_hEditLog, L"");
            }
        } else if ((id == IDC_EDIT_FILTER || id == IDC_EDIT_AIRCRAFT) && HIWORD(wParam) == EN_CHANGE) {
            /* запрос набран - сразу показываем таблицу, чтобы результат был виден, а не только "где-то в логе" */
            if (!g_table_view && GetWindowTextLengthW((HWND)lParam) > 0) {
                g_table_view = 1;
                ShowWindow(g_hEditLog, SW_HIDE);
                ShowWindow(g_hTable, SW_SHOW);
                SetWindowTextW(g_hBtnView, L"\u2261 Лог");
            }
            rebuild_table_view();
        } else if (id == IDC_BTN_VIEW) {
            g_table_view = !g_table_view;
            ShowWindow(g_hEditLog, g_table_view ? SW_HIDE : SW_SHOW);
            ShowWindow(g_hTable, g_table_view ? SW_SHOW : SW_HIDE);
            SetWindowTextW(g_hBtnView, g_table_view ? L"\u2261 Лог" : L"\u25A4 Таблица");
        } else if (id == IDC_BTN_STATS) {
            show_statistics(hwnd);
        } else if (id == IDC_BTN_ROUTES) {
            show_routes(hwnd);
        } else if (id == IDC_BTN_IMPORT) {
            import_jsonl_dialog(hwnd);
        } else if (id == IDC_BTN_EXPORT) {
            export_dialog(hwnd);
        } else if (id == IDC_BTN_FAULTS) {
            show_faults(hwnd);
        } else if (id == IDC_BTN_TELEMETRY) {
            show_telemetry(hwnd);
        } else if (id == IDC_BTN_WEATHER) {
            show_weather(hwnd);
        } else if (id == IDC_BTN_DETAILS) {
            show_details(hwnd);
        } else if (id == IDC_BTN_MAP) {
            show_map(hwnd);
        } else if (id == IDC_BTN_DIAG) {
            check_environment(hwnd);
        } else if (id == IDC_BTN_MEMLIST) {
            show_memory_list(hwnd);
        } else if (id == IDC_CHECK_TOPMOST) {
            BOOL checked = (SendMessageW(g_hCheckTopmost, BM_GETCHECK, 0, 0) == BST_CHECKED);
            SetWindowPos(hwnd, checked ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE);
        } else if (id == IDM_TRAY_SHOW) {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        } else if (id == IDM_TRAY_EXIT) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else if (id == IDC_BTN_FILE) {
            if (!g_file_decoding)
                open_file_dialog(hwnd);
        } else if (id == IDC_BTN_START) {
            if (!g_capturing) {
                int idx = (int)SendMessageW(g_hCombo, CB_GETCURSEL, 0, 0);
                if (idx == CB_ERR) {
                    MessageBoxW(hwnd, L"Сначала выберите устройство записи из списка.",
                                L"Внимание", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                g_stop_flag = 0;
                g_capturing = 1;
                g_capture_thread = CreateThread(NULL, 0, capture_thread_proc,
                                                 (LPVOID)(intptr_t)idx, 0, NULL);
                SetWindowTextW(g_hBtnStart, L"\u25A0 Остановить приём");
                set_status(L"Идёт приём...", CLR_FG_OK);
                EnableWindow(g_hCombo, FALSE);
                EnableWindow(g_hBtnRefresh, FALSE);
            } else {
                g_stop_flag = 1;
                EnableWindow(g_hBtnStart, FALSE);
                set_status(L"Останавливаю приём...", CLR_FG_TEXT);
            }
        } else if (id == IDC_BTN_AR_CONNECT) {
            if (!ar5000_is_connected())
                ar_do_connect(hwnd);
            else
                ar_do_disconnect();
        } else if (id == IDC_BTN_TUNE) {
            ar_do_tune(hwnd);
        } else if (id == IDC_COMBO_PRESET && HIWORD(wParam) == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(g_hComboPreset, CB_GETCURSEL, 0, 0);
            if (sel > 0 && sel <= N_ACARS_PRESETS) {
                WCHAR buf[16];
                format_mhz(buf, 16, kAcarsPresets[sel - 1]);
                SetWindowTextW(g_hEditFreq, buf);
                if (ar5000_is_connected())
                    ar_do_tune(hwnd);
            }
        } else if (id == IDC_COMBO_MODE && HIWORD(wParam) == CBN_SELCHANGE) {
            if (ar5000_is_connected()) {
                int sel = (int)SendMessageW(g_hComboMode, CB_GETCURSEL, 0, 0);
                if (sel >= 0)
                    ar5000_set_mode(sel);
            }
        } else if (id == IDC_COMBO_BW && HIWORD(wParam) == CBN_SELCHANGE) {
            if (ar5000_is_connected()) {
                int sel = (int)SendMessageW(g_hComboBW, CB_GETCURSEL, 0, 0);
                if (sel >= 0)
                    ar5000_set_bandwidth(sel);
            }
        } else if (id == IDC_BTN_SCAN) {
            if (!ar5000_is_connected()) {
                MessageBoxW(hwnd, L"Сначала подключитесь к приёмнику - сканирование переключает частоту по RS-232.",
                            L"Внимание", MB_OK | MB_ICONWARNING);
                return 0;
            }
            if (!g_scan_active) {
                scan_start();
                SetWindowTextW(g_hBtnScan, L"\u25A0 Стоп скан.");
                EnableWindow(g_hEditFreq, FALSE);
                EnableWindow(g_hBtnTune, FALSE);
                EnableWindow(g_hComboPreset, FALSE);
            } else {
                scan_stop();
                SetWindowTextW(g_hBtnScan, L"\u21BB Сканировать");
                EnableWindow(g_hEditFreq, TRUE);
                EnableWindow(g_hBtnTune, TRUE);
                EnableWindow(g_hComboPreset, TRUE);
            }
        }
        return 0;
    }

    case WM_NOTIFY: {
        LPNMHDR nmhdr = (LPNMHDR)lParam;
        if (nmhdr->hwndFrom == g_hTable && nmhdr->code == NM_DBLCLK) {
            /* двойной щелчок по строке - оставить в таблице только сообщения этого борта */
            LPNMITEMACTIVATE ia = (LPNMITEMACTIVATE)lParam;
            if (ia->iItem >= 0) {
                WCHAR addr[240], reg[64];
                addr[0] = 0;
                ListView_GetItemText(g_hTable, ia->iItem, 3, addr, 240);
                size_t k = 0;
                while (addr[k] && addr[k] != L' ' && k < 63) {
                    reg[k] = addr[k];
                    k++;
                }
                reg[k] = 0;
                const WCHAR *r = reg[0] == L'.' ? reg + 1 : reg;
                if (*r)
                    SetWindowTextW(g_hEditAircraft, r); /* EN_CHANGE перестроит таблицу */
            }
            return 0;
        }
        if (nmhdr->hwndFrom == g_hTable && nmhdr->code == LVN_COLUMNCLICK) {
            LPNMLISTVIEW nmlv = (LPNMLISTVIEW)lParam;
            if (nmlv->iSubItem == g_sort_column)
                g_sort_ascending = !g_sort_ascending;
            else {
                g_sort_column = nmlv->iSubItem;
                g_sort_ascending = TRUE;
            }
            ListView_SortItemsEx(g_hTable, table_compare, 0);
            return 0;
        }
        break;
    }

    case WM_APP_LOG: {
        WCHAR *text = (WCHAR *)lParam;
        if (text) {
            append_log(text);
            free(text);
        }
        return 0;
    }

    case WM_APP_TABLE_ROW: {
        TableRow *row = (TableRow *)lParam;
        if (row) {
            store_row(row);
            notify_new_message(row);
            WCHAR filter[256];
            GetWindowTextW(g_hEditFilter, filter, 256);
            WCHAR acf[64];
            read_aircraft_filter(acf, 64);
            if (row_matches_filter(row, filter) && row_matches_aircraft(row, acf))
                table_add_row(row);
            free(row);
        }
        return 0;
    }

    case WM_APP_CAPTURE_STOPPED: {
        if (g_capture_thread) {
            CloseHandle(g_capture_thread);
            g_capture_thread = NULL;
        }
        g_capturing = 0;
        {
            WCHAR start_txt[48];
            wsprintfW(start_txt, L"%s Начать приём", xp_play_glyph());
            SetWindowTextW(g_hBtnStart, start_txt);
        }
        set_status(L"Остановлено.", CLR_FG_LABEL);
        EnableWindow(g_hBtnStart, TRUE);
        EnableWindow(g_hCombo, TRUE);
        EnableWindow(g_hBtnRefresh, TRUE);
        return 0;
    }

    case WM_APP_AUTOLOAD:
        autoload_journals(hwnd);
        return 0;

    case WM_APP_FILE_DONE: {
        g_file_decoding = 0;
        EnableWindow(g_hBtnFile, TRUE);
        set_status(L"Готово.", CLR_FG_LABEL);
        return 0;
    }

    case WM_APP_AR_STATUS: {
        int level = (int)wParam;
        int squelch_open = (int)lParam;
        WCHAR buf[220];
        WCHAR prefix[64];
        if (g_scan_active) {
            WCHAR freqbuf[16];
            format_mhz(freqbuf, 16, g_scan_current_mhz);
            wsprintfW(prefix, L"[\u21BB %s МГц] ", freqbuf);
        } else
            prefix[0] = L'\0';
        if (level < 0) {
            wsprintfW(buf, L"\u25CF %sAR5000: подключен, нет ответа (проверьте кабель/питание)", prefix);
            g_ar_status_color = CLR_FG_ERR;
            if (g_hArProgress)
                SendMessageW(g_hArProgress, PBM_SETPOS, 0, 0);
        } else {
            wsprintfW(buf, L"\u25CF %sAR5000: уровень сигнала %d/255%s", prefix,
                      level, squelch_open ? L" - шумоподавитель ОТКРЫТ (есть сигнал)" : L"");
            g_ar_status_color = squelch_open ? CLR_FG_OK : CLR_FG_LABEL;
            if (g_hArProgress) {
                SendMessageW(g_hArProgress, PBM_SETBARCOLOR, 0,
                             (LPARAM)(squelch_open ? CLR_FG_OK : CLR_FG_DIM));
                SendMessageW(g_hArProgress, PBM_SETPOS, (WPARAM)level, 0);
            }
        }
        SetWindowTextW(g_hArStatus, buf);
        InvalidateRect(g_hArStatus, NULL, TRUE);
        return 0;
    }

    case WM_APP_TRAYICON: {
        /* lParam - код события мыши над значком (тот же набор, что и
         * обычные оконные сообщения мыши, просто переданный через
         * Shell_NotifyIcon вместо прямого клика по окну). */
        if (lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        } else if (lParam == WM_RBUTTONUP) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, IDM_TRAY_SHOW, L"Показать окно");
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"Выход");
            POINT pt;
            GetCursorPos(&pt);
            /* Обязательное действие по документации Shell_NotifyIcon -
             * без него всплывающее меню может не закрыться само при
             * клике мимо него. */
            SetForegroundWindow(hwnd);
            TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);
            PostMessageW(hwnd, WM_NULL, 0, 0);
            DestroyMenu(menu);
        }
        return 0;
    }

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_MINIMIZE && g_tray_added) {
            /* Сворачиваем в трей вместо панели задач - значок в трее
             * уже показывает, что программа работает. При первом таком
             * сворачивании за сессию - поясняем всплывающей подсказкой,
             * чтобы не выглядело так, будто программа просто исчезла. */
            ShowWindow(hwnd, SW_HIDE);
            if (!g_shown_tray_hint) {
                g_shown_tray_hint = 1;
                tray_balloon(L"ACARS-декодер свёрнут в трей",
                             L"Приём продолжается в фоне. Дважды кликните по значку, чтобы открыть окно снова.");
            }
            return 0;
        }
        break;

    case WM_TIMER:
        if (wParam == ID_AUTOSTART_TIMER_AR) {
            KillTimer(hwnd, ID_AUTOSTART_TIMER_AR);
            WCHAR combuf[64];
            GetWindowTextW(g_hEditCom, combuf, 64);
            if (combuf[0] && !ar5000_is_connected())
                PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_BTN_AR_CONNECT, BN_CLICKED), 0);
            /* Начало приёма - отдельным, чуть более поздним таймером,
             * а не сразу же следом: подключение к AR5000 само по себе
             * не мгновенное (блокирующий обмен по COM-порту внутри
             * ar_do_connect), не стоит накладывать его на запуск
             * потока захвата звука в один и тот же момент. */
            SetTimer(hwnd, ID_AUTOSTART_TIMER_CAPTURE, 700, NULL);
            return 0;
        }
        if (wParam == ID_AUTOSTART_TIMER_CAPTURE) {
            KillTimer(hwnd, ID_AUTOSTART_TIMER_CAPTURE);
            if (!g_capturing && SendMessageW(g_hCombo, CB_GETCURSEL, 0, 0) != CB_ERR)
                PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_BTN_START, BN_CLICKED), 0);
            return 0;
        }
        break;

    case WM_CLOSE:
        save_settings(hwnd);
        if (g_capturing) {
            g_stop_flag = 1;
            if (g_capture_thread)
                WaitForSingleObject(g_capture_thread, 1500); /* дать устройству закрыться аккуратно */
        }
        if (ar5000_is_connected())
            ar_do_disconnect(); /* останавливает опрос и шлёт EX приёмнику */
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ID_AUTOSTART_TIMER_AR/CAPTURE определены выше, перед WndProc - они
 * используются внутри неё (обработчик WM_TIMER). */

/* --- Запоминание настроек между запусками (acars_settings.ini) ---
 * Обычный INI-файл через стандартные Get/WritePrivateProfileString -
 * тот же принцип, что и aircraft_db.csv/airport_db.csv: лежит в
 * рабочей папке программы. GetPrivateProfileStringW с ОТНОСИТЕЛЬНЫМ
 * именем файла по старой, ещё досовской традиции Windows ищет файл в
 * системной папке Windows, а не в рабочей папке - поэтому путь строим
 * явно через текущую рабочую папку, а не передаём голое имя файла. */
static void get_settings_path(WCHAR *out, int out_size_chars) {
    WCHAR cwd[512];
    DWORD len = GetCurrentDirectoryW(512, cwd);
    if (len == 0 || len >= 512) {
        wcsncpy(out, L"acars_settings.ini", (size_t)out_size_chars - 1);
        out[out_size_chars - 1] = L'\0';
        return;
    }
    wsprintfW(out, L"%s\\acars_settings.ini", cwd);
}

static void save_settings(HWND hwnd) {
    WCHAR path[600];
    get_settings_path(path, 600);

    WCHAR buf[300];

    GetWindowTextW(g_hEditCom, buf, 300);
    WritePrivateProfileStringW(L"Settings", L"ComPort", buf, path);

    GetWindowTextW(g_hEditFreq, buf, 300);
    WritePrivateProfileStringW(L"Settings", L"Frequency", buf, path);

    wsprintfW(buf, L"%d", (int)SendMessageW(g_hComboBaud, CB_GETCURSEL, 0, 0));
    WritePrivateProfileStringW(L"Settings", L"BaudIndex", buf, path);

    wsprintfW(buf, L"%d", (int)SendMessageW(g_hCombo, CB_GETCURSEL, 0, 0));
    WritePrivateProfileStringW(L"Settings", L"DeviceIndex", buf, path);

    wsprintfW(buf, L"%d", (int)SendMessageW(g_hComboMode, CB_GETCURSEL, 0, 0));
    WritePrivateProfileStringW(L"Settings", L"ModeIndex", buf, path);

    wsprintfW(buf, L"%d", (int)SendMessageW(g_hComboBW, CB_GETCURSEL, 0, 0));
    WritePrivateProfileStringW(L"Settings", L"BandwidthIndex", buf, path);

    wsprintfW(buf, L"%d", (int)SendMessageW(g_hComboMemBank, CB_GETCURSEL, 0, 0));
    WritePrivateProfileStringW(L"Settings", L"MemBankIndex", buf, path);

    wsprintfW(buf, L"%d", SendMessageW(g_hCheckTopmost, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0);
    WritePrivateProfileStringW(L"Settings", L"Topmost", buf, path);

    wsprintfW(buf, L"%d", SendMessageW(g_hCheckNotify, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0);
    WritePrivateProfileStringW(L"Settings", L"Notifications", buf, path);

    wsprintfW(buf, L"%d", SendMessageW(g_hCheckAutostart, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0);
    WritePrivateProfileStringW(L"Settings", L"Autostart", buf, path);

    /* GetWindowPlacement, а не GetWindowRect - если окно сейчас
     * свёрнуто/развёрнуто, нужны координаты ОБЫЧНОГО (restored)
     * состояния, а не того, что видно сейчас. */
    WINDOWPLACEMENT wp;
    wp.length = sizeof(wp);
    if (GetWindowPlacement(hwnd, &wp)) {
        wsprintfW(buf, L"%ld", wp.rcNormalPosition.left);
        WritePrivateProfileStringW(L"Settings", L"WindowX", buf, path);
        wsprintfW(buf, L"%ld", wp.rcNormalPosition.top);
        WritePrivateProfileStringW(L"Settings", L"WindowY", buf, path);
        wsprintfW(buf, L"%ld", wp.rcNormalPosition.right - wp.rcNormalPosition.left);
        WritePrivateProfileStringW(L"Settings", L"WindowWidth", buf, path);
        wsprintfW(buf, L"%ld", wp.rcNormalPosition.bottom - wp.rcNormalPosition.top);
        WritePrivateProfileStringW(L"Settings", L"WindowHeight", buf, path);
    }
}

/* Вызывается из WM_CREATE, уже после создания ВСЕХ элементов
 * управления и после refresh_device_list() (иначе список устройств
 * ещё пуст, и восстановить выбор было бы не из чего). Если файла
 * настроек ещё нет (первый запуск) - просто ничего не делает, все
 * элементы остаются с значениями по умолчанию, как и раньше. */
static void load_settings(HWND hwnd) {
    WCHAR path[600];
    get_settings_path(path, 600);

    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        return;

    WCHAR buf[300];
    int v;

    GetPrivateProfileStringW(L"Settings", L"ComPort", L"", buf, 300, path);
    if (buf[0])
        SetWindowTextW(g_hEditCom, buf);

    GetPrivateProfileStringW(L"Settings", L"Frequency", L"", buf, 300, path);
    if (buf[0])
        SetWindowTextW(g_hEditFreq, buf);

    v = GetPrivateProfileIntW(L"Settings", L"BaudIndex", -1, path);
    if (v >= 0)
        SendMessageW(g_hComboBaud, CB_SETCURSEL, (WPARAM)v, 0);

    v = GetPrivateProfileIntW(L"Settings", L"DeviceIndex", -1, path);
    if (v >= 0 && v < (int)SendMessageW(g_hCombo, CB_GETCOUNT, 0, 0))
        SendMessageW(g_hCombo, CB_SETCURSEL, (WPARAM)v, 0);

    v = GetPrivateProfileIntW(L"Settings", L"ModeIndex", -1, path);
    if (v >= 0)
        SendMessageW(g_hComboMode, CB_SETCURSEL, (WPARAM)v, 0);

    v = GetPrivateProfileIntW(L"Settings", L"BandwidthIndex", -1, path);
    if (v >= 0)
        SendMessageW(g_hComboBW, CB_SETCURSEL, (WPARAM)v, 0);

    v = GetPrivateProfileIntW(L"Settings", L"MemBankIndex", -1, path);
    if (v >= 0)
        SendMessageW(g_hComboMemBank, CB_SETCURSEL, (WPARAM)v, 0);

    v = GetPrivateProfileIntW(L"Settings", L"Topmost", 0, path);
    SendMessageW(g_hCheckTopmost, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);
    if (v)
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    v = GetPrivateProfileIntW(L"Settings", L"Notifications", 0, path);
    SendMessageW(g_hCheckNotify, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);

    int autostart = GetPrivateProfileIntW(L"Settings", L"Autostart", 0, path);
    SendMessageW(g_hCheckAutostart, BM_SETCHECK, autostart ? BST_CHECKED : BST_UNCHECKED, 0);

    int wx = GetPrivateProfileIntW(L"Settings", L"WindowX", -32000, path);
    int wy = GetPrivateProfileIntW(L"Settings", L"WindowY", -32000, path);
    int ww = GetPrivateProfileIntW(L"Settings", L"WindowWidth", 0, path);
    int wh = GetPrivateProfileIntW(L"Settings", L"WindowHeight", 0, path);
    if (wx > -30000 && ww > 100 && wh > 100)
        SetWindowPos(hwnd, NULL, wx, wy, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);

    /* Автостарт - не делаем сразу внутри WM_CREATE (окно ещё даже не
     * показано), а через короткую задержку таймером: сперва
     * подключение к AR5000 (если в полях есть COM-порт), затем, ещё
     * чуть позже, начало приёма - тем же путём, что обычный клик по
     * кнопкам (PostMessageW WM_COMMAND), чтобы не дублировать логику
     * проверок/обработки ошибок, которая уже есть в обработчиках. */
    if (autostart)
        SetTimer(hwnd, ID_AUTOSTART_TIMER_AR, 900, NULL);
}

int run_gui(void) {
    /* Программа собрана как консольное приложение (это позволяет
     * командам вида "acarsdecoder.exe -f файл.wav" работать из
     * cmd.exe/PowerShell как обычно) - из-за этого при запуске
     * двойным кликом Windows на мгновение показывает окно консоли,
     * прежде чем откроется настоящий графический интерфейс. Прячем
     * его сразу. */
    FreeConsole();

    setup_stdout_redirect();
    ar5000_module_init();

    /* aircraft_db уже загружена в main() ДО вызова run_gui() - само
     * сообщение об этом печатаем НИЖЕ, уже после того, как окно
     * реально создано (см. комментарий там): printf() здесь идёт через
     * перенаправленный stdout -> pipe -> поток-читатель ->
     * PostMessage(g_hwnd_main, ...), а g_hwnd_main устанавливается
     * только внутри WM_CREATE, которое произойдёт только после
     * CreateWindowW ниже. Если напечатать прямо тут - поток-читатель
     * почти наверняка успеет забрать текст из pipe и попытается
     * post-нуть его в g_hwnd_main ДО того, как эта переменная вообще
     * будет установлена (равна 0/NULL) - PostMessage с NULL шлёт
     * сообщение в очередь ВЫЗЫВАЮЩЕГО потока (потока-читателя), а не в
     * окно, и сообщение теряется безвозвратно. Именно поэтому строка
     * "База бортов: ..." раньше не показывалась НИКОГДА, независимо от
     * того, нашёлся файл или нет. */

    /* ICC_LISTVIEW_CLASSES - регистрирует класс окна SysListView32
     * (таблица сообщений). На системах без Common Controls 6 (совсем
     * старых) INITCOMMONCONTROLSEX может не найти нужную версию - тогда
     * вызов просто ничего не сделает, а CreateWindowW для таблицы ниже
     * не найдёт класс и вернёт NULL; сам текстовый лог при этом
     * продолжит работать как обычно. */
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);

    g_hbrMainBg = CreateSolidBrush(CLR_BG_MAIN);
    g_hbrLogBg = CreateSolidBrush(CLR_BG_LOG);

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"AcarsDecoderGuiWndClass";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = g_hbrMainBg;
    /* Свой значок (resources/acars.ico, встроен через resources/acars.rc)
     * - виден в Проводнике, на панели задач, в Alt-Tab. Если по какой-то
     * причине ресурс не загрузился (например, кто-то собрал .exe без
     * компиляции .rc) - откатываемся на системный значок по умолчанию,
     * чтобы окно в любом случае открылось. */
    HICON hAppIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIcon = hAppIcon ? hAppIcon : LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);

    if (!RegisterClassW(&wc)) {
        /* Не молчим при ошибке - раньше при сбое здесь окно просто не
         * появлялось вообще без единого сообщения, что выглядело как
         * "ничего не происходит". Теперь хотя бы понятно, что пошло не
         * так и с каким кодом ошибки Windows. */
        WCHAR errbuf[256];
        wsprintfW(errbuf, L"Не удалось зарегистрировать класс окна (код ошибки %lu).",
                  (unsigned long)GetLastError());
        MessageBoxW(NULL, errbuf, L"ACARS-декодер - ошибка запуска", MB_OK | MB_ICONERROR);
        return 1;
    }

    HWND hwnd = CreateWindowW(L"AcarsDecoderGuiWndClass",
                               L"\u2708 ACARS-декодер для AOR AR5000",
                               WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 1250, 720,
                               NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd) {
        WCHAR errbuf[256];
        wsprintfW(errbuf, L"Не удалось создать окно программы (код ошибки %lu).",
                  (unsigned long)GetLastError());
        MessageBoxW(NULL, errbuf, L"ACARS-декодер - ошибка запуска", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd); /* на случай, если окно откроется не на переднем плане */

    tray_add(hwnd, hAppIcon ? hAppIcon : LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION));

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    tray_remove();
    return 0;
}

#endif /* _WIN32 */
