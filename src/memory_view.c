/*
 * Окно "Банки памяти AR5000".
 *
 * Раньше кнопка "Список памяти" показывала только занятые каналы одного банка текстом: нельзя было понять, что
 * приёмник реально ответил, сколько строк получено, где пусто, а где ответ не распознан. Здесь:
 *  - чтение идёт блоками по 10 каналов по таймеру, окно не зависает, виден прогресс и можно остановить;
 *  - таблица с колонками банк/канал/частота/режим/шаг/пропуск/полоса/аттенюатор/комментарий и сырой строкой ответа;
 *  - журнал обмена "-> команда / <- строка ответа" с предупреждениями (неполный блок, чужой банк, нераспознанная
 *    строка), по нему видно, что именно происходит на порту;
 *  - сводка по банкам в списке выбора ("Банк 3 - занято 27");
 *  - двойной щелчок по каналу - настройка приёмника (MR) с проверкой, какую частоту он на самом деле установил;
 *  - фильтр по тексту, показ/скрытие пустых каналов, сортировка по колонкам, экспорт в CSV.
 */
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include "memory_view.h"
#include "ar5000.h"
#include "xpcompat.h"

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

#define IDC_MV_BANK 701
#define IDC_MV_READ 702
#define IDC_MV_STOP 703
#define IDC_MV_EMPTY 704
#define IDC_MV_FILTER 705
#define IDC_MV_TUNE 706
#define IDC_MV_CSV 707
#define IDC_MV_LIST 708
#define IDC_MV_STATUS 709
#define IDC_MV_LOG 710
#define IDC_MV_CLEARLOG 711
#define TIMER_MV_READ 7101

typedef struct {
    Ar5000MemoryEntry e;
    int state; /* 0 - не читали, 1 - ответ получен */
} Slot;

static Slot g_mem[10][100];
static int g_bad[10];      /* нераспознанных строк по банкам */
static int g_view[1000];   /* что показано в таблице: bank*100+channel */
static int g_nview;
static int g_sort_col = 0, g_sort_asc = 1;
static int g_show_empty;
static WCHAR g_filter[64];

static HWND g_wnd, g_cmb, g_btnRead, g_btnStop, g_chkEmpty, g_edFilter, g_btnTune, g_btnCsv, g_list, g_stat, g_log;
static HFONT g_font, g_fontMono;
static MemTuneFn g_tune_cb;

/* состояние чтения */
static int g_rd_on, g_rd_bank, g_rd_last, g_rd_block, g_rd_fail, g_rd_blocks_ok, g_rd_short;
static DWORD g_rd_t0;

static const WCHAR *kModes[] = {L"FM", L"AM", L"LSB", L"USB", L"CW", L"Sync AM", L"SAL", L"SAH"};
static const WCHAR *kBw[] = {L"0.5 кГц", L"3 кГц", L"6 кГц", L"15 кГц", L"30 кГц", L"110 кГц", L"220 кГц"};

/* ---------- журнал ---------- */
static void log_w(const WCHAR *s) {
    if (!g_log) return;
    if (GetWindowTextLengthW(g_log) > 120000)
        SetWindowTextW(g_log, L"");
    SendMessageW(g_log, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)s);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
}

static void log_a(const char *prefix, const char *s) {
    WCHAR w[300], p[16];
    MultiByteToWideChar(CP_ACP, 0, prefix, -1, p, 16);
    int n = (int)wcslen(p);
    wcscpy(w, p);
    MultiByteToWideChar(CP_ACP, 0, s, -1, w + n, 300 - n);
    log_w(w);
}

static void logf_w(const WCHAR *fmt, ...) {
    WCHAR b[300];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(b, 300, fmt, ap);
    b[299] = 0;
    va_end(ap);
    log_w(b);
}

/* ---------- форматирование ---------- */
static void fmt_mhz(WCHAR *out, size_t cap, uint32_t hz) {
    WCHAR t[32];
    _snwprintf(t, 32, L"%u.%06u", (unsigned)(hz / 1000000u), (unsigned)(hz % 1000000u));
    t[31] = 0;
    size_t n = wcslen(t);
    while (n > 0 && t[n - 1] == L'0' && (long)n > (long)(wcschr(t, L'.') - t) + 4) t[--n] = 0; /* минимум 3 знака после точки */
    wcsncpy(out, t, cap - 1);
    out[cap - 1] = 0;
}

/* Текст ячейки: col 0 банк, 1 канал, 2 частота МГц, 3 режим, 4 шаг кГц, 5 пропуск, 6 полоса, 7 аттенюатор,
 * 8 комментарий, 9 ответ приёмника. */
static void cell_text(const Slot *s, int col, WCHAR *out, size_t cap) {
    const Ar5000MemoryEntry *e = &s->e;
    out[0] = 0;
    switch (col) {
    case 0: _snwprintf(out, cap, L"%d", e->bank); break;
    case 1: _snwprintf(out, cap, L"%02d", e->channel); break;
    case 2: if (e->occupied) fmt_mhz(out, cap, e->freq_hz); else wcsncpy(out, L"-", cap - 1); break;
    case 3:
        if (!e->occupied) break;
        if (e->mode >= 0 && e->mode < 8) wcsncpy(out, kModes[e->mode], cap - 1);
        else if (e->mode >= 0) _snwprintf(out, cap, L"? (%d)", e->mode);
        break;
    case 4:
        if (e->occupied && e->step_hz) {
            if (e->step_hz % 1000 == 0) _snwprintf(out, cap, L"%u", (unsigned)(e->step_hz / 1000));
            else _snwprintf(out, cap, L"%u.%03u", (unsigned)(e->step_hz / 1000), (unsigned)(e->step_hz % 1000));
        }
        break;
    case 5:
        if (e->occupied && e->pass >= 0) wcsncpy(out, e->pass ? L"пропуск" : L"нет", cap - 1);
        break;
    case 6:
        if (e->occupied && e->bw >= 0) {
            if (e->bw < 7) wcsncpy(out, kBw[e->bw], cap - 1);
            else _snwprintf(out, cap, L"? (%d)", e->bw);
        }
        break;
    case 7:
        if (e->occupied && e->att >= 0) _snwprintf(out, cap, L"%d", e->att);
        break;
    case 8: MultiByteToWideChar(CP_ACP, 0, e->comment, -1, out, (int)cap); break;
    case 9: MultiByteToWideChar(CP_ACP, 0, e->raw, -1, out, (int)cap); break;
    }
    out[cap - 1] = 0;
}

static int ci_has(const WCHAR *h, const WCHAR *n) {
    size_t hn = wcslen(h), nn = wcslen(n);
    if (!nn) return 1;
    if (nn > hn) return 0;
    for (size_t i = 0; i + nn <= hn; i++) {
        size_t j = 0;
        while (j < nn && towlower(h[i + j]) == towlower(n[j])) j++;
        if (j == nn) return 1;
    }
    return 0;
}

static int slot_matches(const Slot *s) {
    if (!g_filter[0]) return 1;
    for (int c = 0; c < 10; c++) {
        if (c == 0 || c == 1) continue;
        WCHAR t[160];
        cell_text(s, c, t, 160);
        if (ci_has(t, g_filter)) return 1;
    }
    return 0;
}

static int cmp_view(const void *a, const void *b) {
    int ia = *(const int *)a, ib = *(const int *)b;
    const Slot *sa = &g_mem[ia / 100][ia % 100], *sb = &g_mem[ib / 100][ib % 100];
    int r = 0;
    switch (g_sort_col) {
    case 0: case 1: r = ia - ib; break;
    case 2: r = (sa->e.occupied ? (long long)sa->e.freq_hz : -1) < (sb->e.occupied ? (long long)sb->e.freq_hz : -1) ? -1
              : ((sa->e.occupied ? (long long)sa->e.freq_hz : -1) > (sb->e.occupied ? (long long)sb->e.freq_hz : -1) ? 1 : 0);
        break;
    default: {
        WCHAR x[160], y[160];
        cell_text(sa, g_sort_col, x, 160);
        cell_text(sb, g_sort_col, y, 160);
        r = _wcsicmp(x, y);
    }
    }
    if (r == 0) r = ia - ib;
    return g_sort_asc ? r : -r;
}

static int cur_scope(int *b0, int *b1) {
    int sel = (int)SendMessageW(g_cmb, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel > 9) { *b0 = 0; *b1 = 9; return 1; }
    *b0 = *b1 = sel;
    return 0;
}

static void update_status(void) {
    int b0, b1;
    cur_scope(&b0, &b1);
    int total = 0, occ = 0, emp = 0, bad = 0;
    for (int b = b0; b <= b1; b++) {
        bad += g_bad[b];
        for (int c = 0; c < 100; c++)
            if (g_mem[b][c].state) {
                total++;
                if (g_mem[b][c].e.occupied) occ++; else emp++;
            }
    }
    WCHAR t[300];
    _snwprintf(t, 300, L"%s  Прочитано каналов: %d из %d  |  занято: %d  |  пусто: %d  |  нераспознанных строк: %d  |  показано в таблице: %d%s",
               g_rd_on ? L"[чтение...]" : L"", total, (b1 - b0 + 1) * 100, occ, emp, bad, g_nview,
               (total == 0 && !g_rd_on) ? L"  (нажмите «Прочитать»)" : L"");
    t[299] = 0;
    SetWindowTextW(g_stat, t);
}

static void update_bank_combo(void) {
    int sel = (int)SendMessageW(g_cmb, CB_GETCURSEL, 0, 0);
    SendMessageW(g_cmb, CB_RESETCONTENT, 0, 0);
    for (int b = 0; b < 10; b++) {
        int rd = 0, occ = 0;
        for (int c = 0; c < 100; c++)
            if (g_mem[b][c].state) { rd++; if (g_mem[b][c].e.occupied) occ++; }
        WCHAR t[80];
        if (rd == 0) _snwprintf(t, 80, L"Банк %d", b);
        else if (rd < 100) _snwprintf(t, 80, L"Банк %d - занято %d (прочитано %d из 100)", b, occ, rd);
        else _snwprintf(t, 80, L"Банк %d - занято %d из 100", b, occ);
        t[79] = 0;
        SendMessageW(g_cmb, CB_ADDSTRING, 0, (LPARAM)t);
    }
    SendMessageW(g_cmb, CB_ADDSTRING, 0, (LPARAM)L"Все банки (0-9)");
    SendMessageW(g_cmb, CB_SETCURSEL, (WPARAM)(sel < 0 ? 0 : sel), 0);
}

static void rebuild_view(void) {
    int b0, b1;
    cur_scope(&b0, &b1);
    g_nview = 0;
    for (int b = b0; b <= b1; b++)
        for (int c = 0; c < 100; c++) {
            const Slot *s = &g_mem[b][c];
            if (!s->state) continue;
            if (!s->e.occupied && !g_show_empty) continue;
            if (!slot_matches(s)) continue;
            g_view[g_nview++] = b * 100 + c;
        }
    qsort(g_view, (size_t)g_nview, sizeof(int), cmp_view);
    SendMessageW(g_list, LVM_SETITEMCOUNT, (WPARAM)g_nview, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    InvalidateRect(g_list, NULL, TRUE);
    update_status();
}

/* ---------- чтение ---------- */
static void rd_stop(const WCHAR *why) {
    if (!g_rd_on) return;
    g_rd_on = 0;
    KillTimer(g_wnd, TIMER_MV_READ);
    EnableWindow(g_btnRead, TRUE);
    EnableWindow(g_btnStop, FALSE);
    EnableWindow(g_cmb, TRUE);
    logf_w(L"=== %s (%lu мс, блоков принято: %d, неполных: %d) ===", why, (unsigned long)(GetTickCount() - g_rd_t0),
           g_rd_blocks_ok, g_rd_short);
    update_bank_combo();
    rebuild_view();
}

static void rd_start(void) {
    if (!ar5000_is_connected()) {
        MessageBoxW(g_wnd, L"Сначала подключитесь к приёмнику в главном окне - память читается по RS-232.",
                    L"Банки памяти", MB_OK | MB_ICONWARNING);
        return;
    }
    int b0, b1;
    cur_scope(&b0, &b1);
    for (int b = b0; b <= b1; b++) {
        memset(g_mem[b], 0, sizeof g_mem[b]);
        g_bad[b] = 0;
    }
    g_rd_bank = b0;
    g_rd_last = b1;
    g_rd_block = 0;
    g_rd_fail = g_rd_blocks_ok = g_rd_short = 0;
    g_rd_t0 = GetTickCount();
    g_rd_on = 1;
    EnableWindow(g_btnRead, FALSE);
    EnableWindow(g_btnStop, TRUE);
    EnableWindow(g_cmb, FALSE);
    logf_w(L"=== Чтение банков %d-%d: команда MA, по 10 каналов за запрос ===", b0, b1);
    rebuild_view();
    SetTimer(g_wnd, TIMER_MV_READ, 30, NULL);
}

static void rd_step(void) {
    if (!g_rd_on) return;
    KillTimer(g_wnd, TIMER_MV_READ); /* чтение блокирует до ~секунды: следующий шаг ставим после него */
    int bank = g_rd_bank, start = g_rd_block * 10;
    Ar5000MemoryEntry ent[10];
    char cmd[16];
    snprintf(cmd, sizeof cmd, "MA%d%02d", bank, start);
    log_a("-> ", cmd);
    int n = ar5000_read_memory_block(bank, start, ent, 10);
    if (n < 0) {
        rd_stop(L"Остановлено: ошибка записи в порт");
        return;
    }
    if (n == 0) {
        g_rd_fail++;
        log_w(L"<- (нет ответа: тайм-аут)");
        if (g_rd_fail >= 2) {
            rd_stop(L"Остановлено: приёмник не отвечает на MA (проверьте кабель, скорость и режим REMOTE)");
            MessageBoxW(g_wnd,
                        L"Приёмник не ответил на запрос списка памяти (MA) дважды подряд.\n\n"
                        L"Проверьте: подключение в главном окне, скорость порта (она должна совпадать с CONFIG приёмника), "
                        L"кабель. Подробности - в журнале обмена внизу окна.",
                        L"Банки памяти", MB_OK | MB_ICONWARNING);
            return;
        }
        SetTimer(g_wnd, TIMER_MV_READ, 200, NULL); /* повторяем тот же блок */
        return;
    }
    g_rd_fail = 0;
    for (int i = 0; i < n; i++) {
        log_a("<- ", ent[i].raw);
        if (ent[i].channel < 0) {
            g_bad[bank]++;
            log_w(L"   ! строка не похожа на \"MXmnn ...\" - не распознана");
            continue;
        }
        if (ent[i].bank != bank)
            logf_w(L"   ! в ответе банк %d, а запрошен %d", ent[i].bank, bank);
        if (ent[i].channel < start || ent[i].channel >= start + 10)
            logf_w(L"   ! канал %02d вне запрошенного диапазона %02d-%02d", ent[i].channel, start, start + 9);
        if (ent[i].bank >= 0 && ent[i].bank < 10 && ent[i].channel < 100) {
            g_mem[ent[i].bank][ent[i].channel].e = ent[i];
            g_mem[ent[i].bank][ent[i].channel].state = 1;
        }
    }
    if (n < 10) {
        g_rd_short++;
        logf_w(L"   ! получено %d строк из 10 - ответ неполный (приёмник не успел или обрыв связи)", n);
    } else
        g_rd_blocks_ok++;

    if (++g_rd_block >= 10) {
        g_rd_block = 0;
        update_bank_combo();
        if (++g_rd_bank > g_rd_last) {
            rd_stop(L"Чтение завершено");
            return;
        }
    }
    rebuild_view();
    SetTimer(g_wnd, TIMER_MV_READ, 20, NULL);
}

/* ---------- действия над каналом ---------- */
static const Slot *selected_slot(void) {
    int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (i < 0 || i >= g_nview) return NULL;
    int v = g_view[i];
    return &g_mem[v / 100][v % 100];
}

static void do_tune(void) {
    const Slot *s = selected_slot();
    if (!s) {
        MessageBoxW(g_wnd, L"Выберите канал в таблице.", L"Банки памяти", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!s->e.occupied) {
        MessageBoxW(g_wnd, L"Этот канал пуст - настраиваться не на что.", L"Банки памяти", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!ar5000_is_connected()) {
        MessageBoxW(g_wnd, L"Приёмник не подключён.", L"Банки памяти", MB_OK | MB_ICONWARNING);
        return;
    }
    int b = s->e.bank, c = s->e.channel;
    logf_w(L"-> MR%d%02d (настроить на канал)", b, c);
    if (ar5000_recall_memory(b, c) != 0) {
        log_w(L"   ! команда не отправлена");
        return;
    }
    Sleep(150);
    uint32_t hz = 0;
    if (ar5000_get_frequency_hz(&hz) != 0) {
        log_w(L"<- RF: приёмник не ответил на запрос частоты - настройка не подтверждена");
        hz = 0;
    } else {
        WCHAR f1[32], f2[32];
        fmt_mhz(f1, 32, hz);
        fmt_mhz(f2, 32, s->e.freq_hz);
        if (hz == s->e.freq_hz)
            logf_w(L"<- RF %s МГц - совпадает с памятью, приёмник настроен", f1);
        else
            logf_w(L"<- RF %s МГц, а в памяти %s МГц - РАСХОЖДЕНИЕ (канал мог быть изменён или команда не сработала)", f1, f2);
    }
    if (g_tune_cb)
        g_tune_cb(hz ? hz : s->e.freq_hz, s->e.mode);
}

static void do_csv(void) {
    if (g_nview == 0) {
        MessageBoxW(g_wnd, L"В таблице нет строк для экспорта.", L"Банки памяти", MB_OK | MB_ICONINFORMATION);
        return;
    }
    WCHAR path[MAX_PATH] = L"ar5000_memory.csv";
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_wnd;
    ofn.lpstrFilter = L"CSV (*.csv)\0*.csv\0\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"csv";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return;
    FILE *f = _wfopen(path, L"wb");
    if (!f) {
        MessageBoxW(g_wnd, L"Не удалось создать файл.", L"Банки памяти", MB_OK | MB_ICONERROR);
        return;
    }
    fwrite("\xEF\xBB\xBF", 1, 3, f);
    static const WCHAR *hd[] = {L"Банк", L"Канал", L"Частота МГц", L"Режим", L"Шаг кГц", L"Пропуск", L"Полоса",
                                L"Аттенюатор", L"Комментарий", L"Ответ приёмника"};
    for (int c = 0; c < 10; c++) {
        char u[200];
        WideCharToMultiByte(CP_UTF8, 0, hd[c], -1, u, sizeof u, NULL, NULL);
        fprintf(f, "%s%s", u, c < 9 ? ";" : "\r\n");
    }
    for (int i = 0; i < g_nview; i++) {
        const Slot *s = &g_mem[g_view[i] / 100][g_view[i] % 100];
        for (int c = 0; c < 10; c++) {
            WCHAR t[200];
            char u[600];
            cell_text(s, c, t, 200);
            for (WCHAR *p = t; *p; p++)
                if (*p == L';' || *p == L'"' || *p == L'\r' || *p == L'\n') *p = L' ';
            if (c == 2) for (WCHAR *p = t; *p; p++) if (*p == L'.') *p = L','; /* Excel с русской локалью */
            WideCharToMultiByte(CP_UTF8, 0, t, -1, u, sizeof u, NULL, NULL);
            fprintf(f, "%s%s", u, c < 9 ? ";" : "\r\n");
        }
    }
    fclose(f);
    logf_w(L"Экспорт: %d строк -> %s", g_nview, path);
}

/* ---------- окно ---------- */
static void layout(void) {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    int w = rc.right, h = rc.bottom;
    int logh = 130, statush = 20;
    MoveWindow(g_list, 8, 76, w - 16, h - 76 - logh - statush - 12, TRUE);
    MoveWindow(g_stat, 8, h - logh - statush - 6, w - 16, statush, TRUE);
    MoveWindow(g_log, 8, h - logh - 2, w - 16, logh - 4, TRUE);
}

static void add_col(int i, const WCHAR *t, int w) {
    LVCOLUMNW c;
    memset(&c, 0, sizeof c);
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    c.fmt = LVCFMT_LEFT;
    c.pszText = (LPWSTR)t;
    c.cx = w;
    SendMessageW(g_list, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&c);
}

static HWND mk(const WCHAR *cls, const WCHAR *txt, DWORD st, DWORD ex, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | st, x, y, w, h, g_wnd, (HMENU)(INT_PTR)id, NULL, NULL);
    if (c) SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static LRESULT CALLBACK MemWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_wnd = hwnd;
        g_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, RUSSIAN_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, xp_ui_face());
        g_fontMono = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, RUSSIAN_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH, L"Consolas");
        mk(L"STATIC", L"Банк:", 0, 0, 10, 12, 44, 20, 0);
        g_cmb = mk(L"COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL, 0, 56, 8, 300, 260, IDC_MV_BANK);
        g_btnRead = mk(L"BUTTON", L"Прочитать", BS_PUSHBUTTON, 0, 366, 7, 100, 26, IDC_MV_READ);
        g_btnStop = mk(L"BUTTON", L"Стоп", BS_PUSHBUTTON, 0, 472, 7, 70, 26, IDC_MV_STOP);
        EnableWindow(g_btnStop, FALSE);
        g_chkEmpty = mk(L"BUTTON", L"Показывать пустые", BS_AUTOCHECKBOX, 0, 556, 9, 170, 22, IDC_MV_EMPTY);
        mk(L"STATIC", L"Фильтр:", 0, 0, 10, 46, 56, 20, 0);
        g_edFilter = mk(L"EDIT", NULL, ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 66, 42, 220, 24, IDC_MV_FILTER);
        SendMessageW(g_edFilter, EM_SETCUEBANNER, TRUE, (LPARAM)L"частота, комментарий, режим...");
        g_btnTune = mk(L"BUTTON", L"Настроить на канал", BS_PUSHBUTTON, 0, 300, 41, 160, 26, IDC_MV_TUNE);
        g_btnCsv = mk(L"BUTTON", L"Экспорт в CSV...", BS_PUSHBUTTON, 0, 468, 41, 140, 26, IDC_MV_CSV);
        mk(L"BUTTON", L"Очистить журнал", BS_PUSHBUTTON, 0, 616, 41, 130, 26, IDC_MV_CLEARLOG);
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
                                 WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                 0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_MV_LIST, NULL, NULL);
        SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(g_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        add_col(0, L"Банк", 46);
        add_col(1, L"Канал", 56);
        add_col(2, L"Частота, МГц", 110);
        add_col(3, L"Режим", 70);
        add_col(4, L"Шаг, кГц", 70);
        add_col(5, L"Пропуск", 74);
        add_col(6, L"Полоса", 76);
        add_col(7, L"Аттен.", 56);
        add_col(8, L"Комментарий", 110);
        add_col(9, L"Ответ приёмника", 360);
        g_stat = mk(L"STATIC", L"", SS_ENDELLIPSIS, 0, 0, 0, 10, 10, IDC_MV_STATUS);
        g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL,
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                                0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_MV_LOG, NULL, NULL);
        SendMessageW(g_log, WM_SETFONT, (WPARAM)(g_fontMono ? g_fontMono : g_font), TRUE);
        SendMessageW(g_log, EM_SETLIMITTEXT, 400000, 0);
        update_bank_combo();
        log_w(L"Журнал обмена с приёмником: \"->\" команда, \"<-\" строка ответа, \"!\" - замечание.");
        layout();
        rebuild_view();
        return 0;

    case WM_SIZE:
        if (g_list) layout();
        return 0;

    case WM_NOTIFY: {
        NMHDR *h = (NMHDR *)lp;
        if (h->hwndFrom != g_list) break;
        if (h->code == LVN_GETDISPINFOW) {
            NMLVDISPINFOW *d = (NMLVDISPINFOW *)lp;
            if ((d->item.mask & LVIF_TEXT) && d->item.iItem >= 0 && d->item.iItem < g_nview) {
                int v = g_view[d->item.iItem];
                cell_text(&g_mem[v / 100][v % 100], d->item.iSubItem, d->item.pszText, (size_t)d->item.cchTextMax);
            }
            return 0;
        }
        if (h->code == LVN_COLUMNCLICK) {
            NMLISTVIEW *n = (NMLISTVIEW *)lp;
            if (n->iSubItem == g_sort_col) g_sort_asc = !g_sort_asc;
            else { g_sort_col = n->iSubItem; g_sort_asc = 1; }
            rebuild_view();
            return 0;
        }
        if (h->code == NM_DBLCLK) {
            do_tune();
            return 0;
        }
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_MV_BANK:
            if (HIWORD(wp) == CBN_SELCHANGE) rebuild_view();
            return 0;
        case IDC_MV_READ: rd_start(); return 0;
        case IDC_MV_STOP: rd_stop(L"Чтение остановлено вручную"); return 0;
        case IDC_MV_EMPTY:
            g_show_empty = SendMessageW(g_chkEmpty, BM_GETCHECK, 0, 0) == BST_CHECKED;
            rebuild_view();
            return 0;
        case IDC_MV_FILTER:
            if (HIWORD(wp) == EN_CHANGE) {
                GetWindowTextW(g_edFilter, g_filter, 64);
                rebuild_view();
            }
            return 0;
        case IDC_MV_TUNE: do_tune(); return 0;
        case IDC_MV_CSV: do_csv(); return 0;
        case IDC_MV_CLEARLOG: SetWindowTextW(g_log, L""); return 0;
        }
        break;

    case WM_TIMER:
        if (wp == TIMER_MV_READ) {
            rd_step(); /* сам ставит следующий тик */
            return 0;
        }
        break;

    case WM_CLOSE:
        if (g_rd_on) rd_stop(L"Чтение прервано закрытием окна");
        ShowWindow(hwnd, SW_HIDE); /* данные и журнал сохраняются до следующего открытия */
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void memview_show(HWND parent, int initial_bank, MemTuneFn tune_cb) {
    g_tune_cb = tune_cb;
    if (!g_wnd) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = MemWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hIcon = parent ? (HICON)GetClassLongPtrW(parent, GCLP_HICON) : NULL;
        wc.lpszClassName = L"AcarsMemViewClass";
        RegisterClassW(&wc);
        CreateWindowExW(0, L"AcarsMemViewClass", L"Банки памяти AR5000", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                        CW_USEDEFAULT, CW_USEDEFAULT, 980, 660, parent, NULL, GetModuleHandleW(NULL), NULL);
        if (!g_wnd) return;
    }
    SendMessageW(g_cmb, CB_SETCURSEL, (WPARAM)((initial_bank >= 0 && initial_bank <= 9) ? initial_bank : 10), 0);
    rebuild_view();
    ShowWindow(g_wnd, IsIconic(g_wnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_wnd);
}
