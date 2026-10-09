#include "journal_export.h"
#include "aircraft_report.h"
#include "acars_report.h"
#include "aircraft_db.h"
#include "airport_db.h"
#include "fileio.h"
#include "labels.h"
#include "units.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdint.h>

/* ================================================================== */
/* Время                                                               */
/* ================================================================== */

int journal_parse_time(const char *in, int round_up, char out[20]) {
    if (!in) return 0;
    while (*in == ' ') in++;
    int Y = 0, M = 0, D = 0, h = -1, mi = -1, se = -1;
    char date[32] = "", tm[32] = "";
    /* разделяем на дату и время по пробелу / 'T' */
    const char *sp = strpbrk(in, " T");
    size_t dl = sp ? (size_t)(sp - in) : strlen(in);
    if (strchr(in, ':') && (!sp || memchr(in, ':', dl) != NULL)) { /* только время */
        snprintf(tm, sizeof tm, "%s", in);
    } else {
        if (dl >= sizeof date) return 0;
        memcpy(date, in, dl); date[dl] = 0;
        if (sp) { while (*sp == ' ' || *sp == 'T') sp++; snprintf(tm, sizeof tm, "%s", sp); }
    }
    if (date[0]) {
        if (sscanf(date, "%d-%d-%d", &Y, &M, &D) != 3 || Y < 100) {
            if (sscanf(date, "%d.%d.%d", &D, &M, &Y) != 3) return 0;
            if (Y < 100) Y += 2000;
        }
    } else {
        time_t t = time(NULL); struct tm *lt = localtime(&t);
        if (!lt) return 0;
        Y = lt->tm_year + 1900; M = lt->tm_mon + 1; D = lt->tm_mday;
    }
    if (tm[0]) {
        int n = sscanf(tm, "%d:%d:%d", &h, &mi, &se);
        if (n < 1) return 0;
        if (n < 2) mi = round_up ? 59 : 0;
        if (n < 3) se = round_up ? 59 : 0;
    } else {
        h = round_up ? 23 : 0; mi = round_up ? 59 : 0; se = round_up ? 59 : 0;
    }
    if (M < 1 || M > 12 || D < 1 || D > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 59) return 0;
    snprintf(out, 20, "%04d-%02d-%02d %02d:%02d:%02d", Y, M, D, h, mi, se);
    return 1;
}

/* дни от 1899-12-30 (нулевая дата Excel) */
static double excel_serial(int Y, int M, int D, int h, int mi, int se) {
    int y = Y - (M <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468; /* от 1970-01-01 */
    return (double)(days + 25569) + (h * 3600 + mi * 60 + se) / 86400.0;
}

/* ================================================================== */
/* Чтение журнала                                                      */
/* ================================================================== */

static char *read_line(FILE *f, char **buf, size_t *cap) {
    size_t len = 0;
    if (!*buf) { *cap = 4096; *buf = (char *)malloc(*cap); if (!*buf) return NULL; }
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (len + 2 >= *cap) {
            char *nb = (char *)realloc(*buf, *cap * 2);
            if (!nb) return NULL;
            *buf = nb; *cap *= 2;
        }
        if (c == '\n') break;
        (*buf)[len++] = (char)c;
    }
    if (len == 0 && c == EOF) return NULL;
    while (len && (*buf)[len - 1] == '\r') len--;
    (*buf)[len] = 0;
    return *buf;
}

static void put_utf8(char *out, size_t cap, size_t *o, unsigned cp) {
    char t[4]; int n;
    if (cp < 0x80) { t[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { t[0] = (char)(0xC0 | (cp >> 6)); t[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) { t[0] = (char)(0xE0 | (cp >> 12)); t[1] = (char)(0x80 | ((cp >> 6) & 63)); t[2] = (char)(0x80 | (cp & 63)); n = 3; }
    else { t[0] = (char)(0xF0 | (cp >> 18)); t[1] = (char)(0x80 | ((cp >> 12) & 63)); t[2] = (char)(0x80 | ((cp >> 6) & 63)); t[3] = (char)(0x80 | (cp & 63)); n = 4; }
    for (int i = 0; i < n && *o + 1 < cap; i++) out[(*o)++] = t[i];
}

/* значение строкового поля JSON с разбором escape-последовательностей */
static int jx_get(const char *line, const char *key, char *out, size_t cap) {
    out[0] = 0;
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = line;
    for (;;) {
        p = strstr(p, pat);
        if (!p) return 0;
        if (p == line || p[-1] != '\\') break;
        p += strlen(pat);
    }
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    p++;
    size_t o = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': put_utf8(out, cap, &o, '\n'); break;
            case 'r': put_utf8(out, cap, &o, '\r'); break;
            case 't': put_utf8(out, cap, &o, '\t'); break;
            case 'u': {
                unsigned cp = 0; int k = 0;
                while (k < 4 && p[1 + k] && isxdigit((unsigned char)p[1 + k])) {
                    char c = p[1 + k];
                    cp = cp * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
                    k++;
                }
                p += k;
                put_utf8(out, cap, &o, cp);
                break;
            }
            default: put_utf8(out, cap, &o, (unsigned char)*p);
            }
            p++;
        } else {
            if (o + 1 < cap) out[o++] = *p;
            p++;
        }
    }
    out[o] = 0;
    return 1;
}

/* набор хэшей строк - отсев повторов */
typedef struct { uint64_t *t; size_t cap, n; } HSet;
static uint64_t fnv(const char *s) { uint64_t h = 1469598103934665603ULL; for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; } return h | 1; }
static int hset_add(HSet *s, uint64_t h) { /* 1 - новый */
    if (s->n * 2 >= s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 4096;
        uint64_t *nt = (uint64_t *)calloc(nc, sizeof(uint64_t));
        if (!nt) return 1;
        for (size_t i = 0; i < s->cap; i++) if (s->t[i]) { size_t j = s->t[i] % nc; while (nt[j]) j = (j + 1) % nc; nt[j] = s->t[i]; }
        free(s->t); s->t = nt; s->cap = nc;
    }
    size_t j = h % s->cap;
    while (s->t[j]) { if (s->t[j] == h) return 0; j = (j + 1) % s->cap; }
    s->t[j] = h; s->n++;
    return 1;
}

static int is_service(const char *label) {
    return label[0] == '_' || strcmp(label, "Q0") == 0 || strcmp(label, "SQ") == 0;
}

/* Регистрации совпадают, если равны без ведущей точки, дефисов и пробелов, без учёта регистра. */
static int reg_same(const char *a, const char *b) {
    for (;;) {
        while (*a == '.' || *a == '-' || *a == ' ') a++;
        while (*b == '.' || *b == '-' || *b == ' ') b++;
        if (!*a || !*b) return !*a && !*b;
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return 0;
        a++; b++;
    }
}

/* Общий перебор: вызывает cb для каждой подходящей строки. */
typedef int (*RowCb)(const char *line, const char *ts, const char *label, void *ctx);

static int for_each_row(const char *const *in, int n_in, const ExportFilter *flt, ExportStats *st, RowCb cb, void *ctx) {
    HSet seen = {0};
    int opened = 0;
    char *buf = NULL; size_t cap = 0;
    for (int i = 0; i < n_in; i++) {
        FILE *f = fopen_utf8(in[i], "rb");
        if (!f) continue;
        opened++;
        char *line;
        while ((line = read_line(f, &buf, &cap)) != NULL) {
            if (!strstr(line, "\"timestamp\"")) continue;
            st->read++;
            char ts[32], label[8];
            if (!jx_get(line, "timestamp", ts, sizeof ts)) { st->skipped++; continue; }
            {
                /* повтор определяем по содержимому (время, борт, метка, начало текста), а не по всей строке: одно и то же
                 * сообщение может прийти из журнала, из импортированного файла и из памяти программы в разной записи */
                char ad[64], lb[8], tx[1024], key[1200];
                jx_get(line, "addr", ad, sizeof ad);
                jx_get(line, "label", lb, sizeof lb);
                jx_get(line, "text", tx, sizeof tx);
                tx[200] = '\0';
                const char *ar = ad[0] == '.' ? ad + 1 : ad;
                snprintf(key, sizeof key, "%s|%s|%s|%s", ts, ar, lb, tx);
                if (!hset_add(&seen, fnv(key))) { st->dups++; continue; }
            }
            if (flt && flt->from && strcmp(ts, flt->from) < 0) { st->skipped++; continue; }
            if (flt && flt->to && strcmp(ts, flt->to) > 0) { st->skipped++; continue; }
            if (flt && flt->reg && flt->reg[0]) {
                char ad[64];
                jx_get(line, "addr", ad, sizeof ad);
                if (!reg_same(ad, flt->reg)) { st->skipped++; continue; }
            }
            jx_get(line, "label", label, sizeof label);
            if (flt && flt->skip_service && is_service(label)) { st->skipped++; continue; }
            if (cb(line, ts, label, ctx)) st->written++;
        }
        fclose(f);
    }
    free(buf); free(seen.t);
    return opened ? 0 : -1;
}

/* ================================================================== */
/* JSONL                                                               */
/* ================================================================== */

static int jsonl_cb(const char *line, const char *ts, const char *label, void *ctx) {
    (void)ts; (void)label;
    fputs(line, (FILE *)ctx);
    fputc('\n', (FILE *)ctx);
    return 1;
}

int journal_export_jsonl(const char *const *in, int n_in, const char *out_path, const ExportFilter *flt, ExportStats *st) {
    ExportStats z = {0};
    if (!st) st = &z;
    memset(st, 0, sizeof *st);
    FILE *f = fopen_utf8(out_path, "wb");
    if (!f) return -2;
    int r = for_each_row(in, n_in, flt, st, jsonl_cb, f);
    fclose(f);
    return r;
}

/* ================================================================== */
/* Zip (без сжатия) и буфер                                            */
/* ================================================================== */

typedef struct { char *p; size_t n, cap; } Buf;

static void bput(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 65536;
        while (nc < b->n + n + 1) nc *= 2;
        char *np = (char *)realloc(b->p, nc);
        if (!np) return;
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void bstr(Buf *b, const char *s) { bput(b, s, strlen(s)); }

static void bxml(Buf *b, const char *s) { /* экранирование + выброс символов, запрещённых в XML 1.0 */
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '&') bstr(b, "&amp;");
        else if (c == '<') bstr(b, "&lt;");
        else if (c == '>') bstr(b, "&gt;");
        else if (c == '"') bstr(b, "&quot;");
        else if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') { /* пропуск */ }
        else if (c == 0x7f) { /* DEL тоже пропускаем */ }
        else bput(b, (const char *)&c, 1);
    }
}

static uint32_t g_crc_tab[256];
static void crc_init(void) {
    if (g_crc_tab[1]) return;
    for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; g_crc_tab[i] = c; }
}
static uint32_t crc32_buf(const char *d, size_t n) {
    crc_init();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = g_crc_tab[(c ^ (unsigned char)d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

typedef struct { char name[64]; uint32_t crc, size, off; } ZEnt;

static void w16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void w32(FILE *f, uint32_t v) { w16(f, v & 0xFFFF); w16(f, v >> 16); }

static void zip_add(FILE *f, ZEnt *e, int *ne, const char *name, const char *data, size_t n) {
    ZEnt *z = &e[(*ne)++];
    snprintf(z->name, sizeof z->name, "%s", name);
    z->crc = crc32_buf(data, n); z->size = (uint32_t)n; z->off = (uint32_t)ftell(f);
    w32(f, 0x04034b50); w16(f, 20); w16(f, 0x0800 /* UTF-8 */); w16(f, 0);
    w16(f, 0); w16(f, 0x21 /* 1980-01-01 */); w32(f, z->crc); w32(f, z->size); w32(f, z->size);
    w16(f, (unsigned)strlen(name)); w16(f, 0);
    fputs(name, f); fwrite(data, 1, n, f);
}
static void zip_finish(FILE *f, ZEnt *e, int ne) {
    uint32_t cd = (uint32_t)ftell(f);
    for (int i = 0; i < ne; i++) {
        w32(f, 0x02014b50); w16(f, 20); w16(f, 20); w16(f, 0x0800); w16(f, 0); w16(f, 0); w16(f, 0x21);
        w32(f, e[i].crc); w32(f, e[i].size); w32(f, e[i].size);
        w16(f, (unsigned)strlen(e[i].name)); w16(f, 0); w16(f, 0); w16(f, 0); w16(f, 0); w32(f, 0);
        w32(f, e[i].off); fputs(e[i].name, f);
    }
    uint32_t end = (uint32_t)ftell(f);
    w32(f, 0x06054b50); w16(f, 0); w16(f, 0); w16(f, (unsigned)ne); w16(f, (unsigned)ne);
    w32(f, end - cd); w32(f, cd); w16(f, 0);
}

/* ================================================================== */
/* Листы                                                               */
/* ================================================================== */

enum { S_PLAIN = 6, S_HEAD = 1, S_WRAP = 2, S_DATE = 3, S_COORD = 4, S_INT = 5 };

static void col_letter(int c, char *out) { /* 0 -> A */
    if (c < 26) { out[0] = (char)('A' + c); out[1] = 0; }
    else { out[0] = (char)('A' + c / 26 - 1); out[1] = (char)('A' + c % 26); out[2] = 0; }
}

static void cell_str(Buf *b, int row, int col, const char *s, int style) {
    if (!s || !s[0]) return;
    char ref[16], l[4]; col_letter(col, l);
    snprintf(ref, sizeof ref, "%s%d", l, row);
    char t[128];
    snprintf(t, sizeof t, "<c r=\"%s\" s=\"%d\" t=\"inlineStr\"><is><t xml:space=\"preserve\">", ref, style);
    bstr(b, t); bxml(b, s); bstr(b, "</t></is></c>");
}
static void cell_num(Buf *b, int row, int col, double v, int style, int decimals) {
    char t[96], l[4]; col_letter(col, l);
    snprintf(t, sizeof t, "<c r=\"%s%d\" s=\"%d\"><v>%.*f</v></c>", l, row, style, decimals, v);
    bstr(b, t);
}

static const char *kCols[] = {
    "Время (местное)", "Источник", "Приём", "Борт", "Тип самолёта", "Оператор", "Метка", "Что означает метка",
    "Блок", "Рейс", "Авиакомпания", "Событие / тип отчёта", "Откуда", "Куда", "Широта", "Долгота",
    "Высота, м", "Высота, ft", "Топливо (сотни кг?)", "ETA, UTC", "Темп., °C", "Ветер (откуда, скорость)",
    "Время события, UTC", "Расшифровка", "Исходный текст"};
static const int kWidths[] = {21, 9, 12, 11, 24, 24, 7, 38, 6, 10, 18, 34, 34, 34, 10, 10, 10, 10, 11, 9, 9, 15, 11, 70, 50};
#define NCOLS ((int)(sizeof(kCols) / sizeof(kCols[0])))

static const char *kEvent[][2] = {
    {"POS", "Позиционный отчёт"}, {"ETA", "Расчётное время прибытия + позиция"}, {"ALT", "Отчёт ALT (высота)"},
    {"OFF", "Взлёт"}, {"INN", "Прибытие на гейт"}, {"SFI", "Итоговая сводка по рейсу"},
    {"PBS", "Начало буксировки (вероятно)"}, {"ADC", "Двери закрыты (вероятно)"}, {"LDC", "Двери закрыты (вероятно)"},
    {"LDO", "Двери открыты (вероятно)"}, {"APS", "Сводка двигатели/ВСУ"}, {"RTG", "RTG (смысл не подтверждён)"}, {NULL, NULL}};

typedef struct {
    char reg[64];
    char type[200], oper[160], flight[10], airline[48];
    char first[20], last[20];
    int msgs, has_pos, has_alt, alt_ft;
    double lat, lon;
    char pos_time[20];
} AcRec;

typedef struct {
    Buf sheet;
    int row;
    AcRec *ac; int nac, cap_ac;
} XCtx;

static void airport_cell(const char *icao, char *out, size_t cap) {
    out[0] = 0;
    if (!icao[0]) return;
    AirportInfo ap;
    if (airport_db_lookup(icao, &ap) && ap.name[0])
        snprintf(out, cap, "%s - %.60s%s%.40s", icao, ap.name, ap.municipality[0] ? ", " : "", ap.municipality);
    else
        snprintf(out, cap, "%s", icao);
}

static AcRec *find_ac(XCtx *x, const char *reg) {
    for (int i = 0; i < x->nac; i++) if (strcmp(x->ac[i].reg, reg) == 0) return &x->ac[i];
    if (x->nac == x->cap_ac) {
        int nc = x->cap_ac ? x->cap_ac * 2 : 128;
        AcRec *n = (AcRec *)realloc(x->ac, sizeof(AcRec) * (size_t)nc);
        if (!n) return NULL;
        x->ac = n; x->cap_ac = nc;
    }
    AcRec *a = &x->ac[x->nac++];
    memset(a, 0, sizeof *a);
    snprintf(a->reg, sizeof a->reg, "%s", reg);
    return a;
}

static void type_text(char *out, size_t cap, const char *man, const char *mod, const char *code) {
    char t[256];
    snprintf(t, sizeof t, "%.100s%s%.100s", man, man[0] && mod[0] ? " " : "", mod);
    if (code[0] && t[0]) snprintf(out, cap, "%.150s (%.30s)", t, code);
    else snprintf(out, cap, "%.190s", t[0] ? t : code);
}

static int xlsx_cb(const char *line, const char *ts, const char *label, void *ctx) {
    XCtx *x = (XCtx *)ctx;
    static char text[8192], addr[64], src[64], crc[16], desc[1024], bid[8], man[64], mod[96], tcode[16], oper[160], dec[4096];
    jx_get(line, "text", text, sizeof text);
    jx_get(line, "addr", addr, sizeof addr);
    jx_get(line, "source", src, sizeof src);
    jx_get(line, "crc_status", crc, sizeof crc);
    jx_get(line, "label_description", desc, sizeof desc);
    jx_get(line, "block_id", bid, sizeof bid);
    jx_get(line, "aircraft_manufacturer", man, sizeof man);
    jx_get(line, "aircraft_model", mod, sizeof mod);
    jx_get(line, "aircraft_typecode", tcode, sizeof tcode);
    jx_get(line, "aircraft_operator", oper, sizeof oper);

    const char *reg = addr[0] == '.' ? addr + 1 : addr;
    char typ[200] = "";
    type_text(typ, sizeof typ, man, mod, tcode);
    if (!typ[0] && reg[0]) {
        AircraftInfo ai;
        if (aircraft_db_lookup(reg, &ai)) {
            type_text(typ, sizeof typ, ai.manufacturer, ai.model, ai.typecode);
            if (!oper[0]) snprintf(oper, sizeof oper, "%s", ai.operator_name);
        }
    }

    AcarsSummary sm;
    acars_report_summary(label, text, &sm);
    dec[0] = 0;
    acars_report_decode(label, text, dec, sizeof dec);

    int Y, M, D, h, mi, se;
    int row = ++x->row;
    char ref[32];
    Buf *b = &x->sheet;
    snprintf(ref, sizeof ref, "<row r=\"%d\">", row);
    bstr(b, ref);
    if (sscanf(ts, "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &mi, &se) == 6)
        cell_num(b, row, 0, excel_serial(Y, M, D, h, mi, se), S_DATE, 8);
    else cell_str(b, row, 0, ts, S_PLAIN);
    cell_str(b, row, 1, strcmp(src, "live") == 0 ? "эфир" : src, S_PLAIN);
    const char *crcru = strcmp(crc, "OK") == 0 ? "верно" : strcmp(crc, "FIXED1") == 0 ? "исправлен 1 бит" :
                        strcmp(crc, "FIXED2") == 0 ? "исправлено 2 бита" : crc;
    cell_str(b, row, 2, crcru, S_PLAIN);
    cell_str(b, row, 3, reg[0] ? reg : "(нет адреса)", S_PLAIN);
    cell_str(b, row, 4, typ, S_PLAIN);
    cell_str(b, row, 5, oper, S_PLAIN);
    {
        char lb[8]; snprintf(lb, sizeof lb, "%s", label);
        for (char *q = lb; *q; q++) if ((unsigned char)*q < 0x20 || *q == 0x7f) *q = '\0';
        if (label[0] == '_') snprintf(lb, sizeof lb, "_ (ACK)");
        cell_str(b, row, 6, lb, S_PLAIN);
    }
    cell_str(b, row, 7, label[0] ? acars_label_lookup(label) : desc, S_WRAP);
    cell_str(b, row, 8, bid, S_PLAIN);
    cell_str(b, row, 9, sm.flight, S_PLAIN);
    cell_str(b, row, 10, sm.airline, S_PLAIN);
    {
        const char *ev = "";
        for (int i = 0; kEvent[i][0]; i++) if (strcmp(kEvent[i][0], sm.type) == 0) ev = kEvent[i][1];
        char evt[80] = "";
        if (sm.type[0]) snprintf(evt, sizeof evt, "%s - %s", sm.type, ev[0] ? ev : "тип не подтверждён");
        cell_str(b, row, 11, evt, S_PLAIN);
    }
    char ap[200];
    airport_cell(sm.dep, ap, sizeof ap);  cell_str(b, row, 12, ap, S_PLAIN);
    airport_cell(sm.dest, ap, sizeof ap); cell_str(b, row, 13, ap, S_PLAIN);
    if (sm.has_pos) { cell_num(b, row, 14, sm.lat, S_COORD, 4); cell_num(b, row, 15, sm.lon, S_COORD, 4); }
    if (sm.has_alt) {
        cell_num(b, row, 16, floor(sm.alt_ft * FEET_TO_METERS + 0.5), S_INT, 0);
        cell_num(b, row, 17, sm.alt_ft, S_INT, 0);
    }
    cell_str(b, row, 18, sm.fuel, S_PLAIN);
    cell_str(b, row, 19, sm.eta, S_PLAIN);
    cell_str(b, row, 20, sm.temp, S_PLAIN);
    cell_str(b, row, 21, sm.wind, S_PLAIN);
    cell_str(b, row, 22, sm.event_time, S_PLAIN);
    cell_str(b, row, 23, dec, S_WRAP);
    cell_str(b, row, 24, text, S_WRAP);
    bstr(b, "</row>");

    if (reg[0] && !is_service(label)) {
        AcRec *a = find_ac(x, reg);
        if (a) {
            if (!a->msgs) snprintf(a->first, sizeof a->first, "%s", ts);
            snprintf(a->last, sizeof a->last, "%s", ts);
            a->msgs++;
            if (typ[0]) snprintf(a->type, sizeof a->type, "%s", typ);
            if (oper[0]) snprintf(a->oper, sizeof a->oper, "%s", oper);
            if (sm.flight[0]) { snprintf(a->flight, sizeof a->flight, "%s", sm.flight); snprintf(a->airline, sizeof a->airline, "%s", sm.airline); }
            if (sm.has_pos) { a->has_pos = 1; a->lat = sm.lat; a->lon = sm.lon; snprintf(a->pos_time, sizeof a->pos_time, "%s", ts); }
            if (sm.has_alt) { a->has_alt = 1; a->alt_ft = sm.alt_ft; }
        }
    }
    return 1;
}

static void sheet_head(Buf *b, int ncols, const int *widths, int freeze_row) {
    bstr(b, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">");
    if (freeze_row)
        bstr(b, "<sheetViews><sheetView workbookViewId=\"0\"><pane ySplit=\"1\" topLeftCell=\"A2\" activePane=\"bottomLeft\" state=\"frozen\"/></sheetView></sheetViews>");
    bstr(b, "<sheetFormatPr defaultRowHeight=\"15\"/><cols>");
    for (int i = 0; i < ncols; i++) {
        char t[96]; snprintf(t, sizeof t, "<col min=\"%d\" max=\"%d\" width=\"%d\" customWidth=\"1\"/>", i + 1, i + 1, widths[i]);
        bstr(b, t);
    }
    bstr(b, "</cols><sheetData>");
}

static void head_row(Buf *b, const char *const *cols, int n) {
    bstr(b, "<row r=\"1\" ht=\"32\" customHeight=\"1\">");
    for (int i = 0; i < n; i++) cell_str(b, 1, i, cols[i], S_HEAD);
    bstr(b, "</row>");
}

static const char kStyles[] =
"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
"<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
"<numFmts count=\"2\"><numFmt numFmtId=\"164\" formatCode=\"dd\\.mm\\.yyyy\\ hh:mm:ss\"/><numFmt numFmtId=\"165\" formatCode=\"0.0000\"/></numFmts>"
"<fonts count=\"2\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font><font><b/><sz val=\"11\"/><color rgb=\"FFFFFFFF\"/><name val=\"Calibri\"/></font></fonts>"
"<fills count=\"3\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill patternType=\"gray125\"/></fill>"
"<fill><patternFill patternType=\"solid\"><fgColor rgb=\"FF1F3864\"/><bgColor indexed=\"64\"/></patternFill></fill></fills>"
"<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
"<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
"<cellXfs count=\"7\">"
"<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>"
"<xf numFmtId=\"0\" fontId=\"1\" fillId=\"2\" borderId=\"0\" xfId=\"0\" applyFont=\"1\" applyFill=\"1\" applyAlignment=\"1\"><alignment horizontal=\"center\" vertical=\"center\" wrapText=\"1\"/></xf>"
"<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyAlignment=\"1\"><alignment vertical=\"top\" wrapText=\"1\"/></xf>"
"<xf numFmtId=\"164\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\" applyAlignment=\"1\"><alignment vertical=\"top\"/></xf>"
"<xf numFmtId=\"165\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\" applyAlignment=\"1\"><alignment vertical=\"top\"/></xf>"
"<xf numFmtId=\"3\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\" applyAlignment=\"1\"><alignment vertical=\"top\"/></xf>"
"<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyAlignment=\"1\"><alignment vertical=\"top\"/></xf>"
"</cellXfs><cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles></styleSheet>";

static const char *kLegend[][2] = {
    {"Время (местное)", "Когда сообщение принято программой - по часам компьютера (для МСК это UTC+3). Это настоящая дата-время Excel: по ней можно сортировать и фильтровать."},
    {"Приём", "Результат проверки контрольной суммы кадра: «верно» - принят без ошибок; «исправлен N бит» - ошибка исправлена кодом коррекции."},
    {"Борт", "Регистрация борта из заголовка ACARS. У служебных сигналов наземных станций адреса нет."},
    {"Тип самолёта / Оператор", "Из базы aircraft_db.csv (если она загружена) или записаны в журнале в момент приёма."},
    {"Метка / Что означает метка", "Двухсимвольный код типа сообщения (ARINC 620) и его описание. У многих авиакомпаний значения меток свои - описание ориентировочное."},
    {"Рейс / Авиакомпания", "Идентификатор рейса из заголовка сообщения (код IATA + номер) и название авиакомпании по этому коду."},
    {"Событие / тип отчёта", "Для отчётов Аэрофлота/Россия: POS, ETA, OFF, INN, SFI... Пометка «вероятно» - смысл определён по порядку событий, документации нет."},
    {"Откуда / Куда", "Аэропорты вылета и назначения (ICAO) с названием, если загружена база аэропортов."},
    {"Широта / Долгота", "Градусы десятичной дробью; юг и запад - со знаком минус."},
    {"Высота, м / Высота, ft", "Борт передаёт высоту в футах (предположительно). Метры - пересчёт: 1 ft = 0.3048 м."},
    {"Топливо", "Остаток топлива как передан бортом, число из сообщения. Единицы документацией не подтверждены (вероятно, сотни кг)."},
    {"Ветер", "Направление, ОТКУДА дует ветер (от севера по часовой стрелке, в скобках сторона света: С, СВ, В...), и скорость в узлах с пересчётом в км/ч и м/с. Значение направления читается как сотые доли градуса, единицы скорости - вероятно узлы: документацией не подтверждено, но скорость растёт с высотой (5 уз у земли, 75-85 уз на 35000 ft), как у реального ветра."},
    {"Время события, UTC", "Время самого события в сообщении (может быть раньше времени приёма - борт отправляет позже)."},
    {"Расшифровка", "Пояснение по-русски: что означает каждая часть текста, и что не удалось расшифровать."},
    {"Исходный текст", "Текст сообщения без изменений."},
    {"Лист «Борта»", "Сводка по каждому борту за выбранный период: сколько сообщений, первое/последнее время, последний рейс, последняя позиция и высота."},
    {NULL, NULL}};

static int sort_ac(const void *a, const void *b) { return strcmp(((const AcRec *)b)->last, ((const AcRec *)a)->last); }

int journal_export_xlsx(const char *const *in, int n_in, const char *out_path, const ExportFilter *flt, ExportStats *st) {
    ExportStats z = {0};
    if (!st) st = &z;
    memset(st, 0, sizeof *st);
    FILE *f = fopen_utf8(out_path, "wb");
    if (!f) return -2;

    XCtx x; memset(&x, 0, sizeof x);
    x.row = 1;
    sheet_head(&x.sheet, NCOLS, kWidths, 1);
    head_row(&x.sheet, kCols, NCOLS);
    int r = for_each_row(in, n_in, flt, st, xlsx_cb, &x);
    if (r != 0) { fclose(f); remove(out_path); free(x.sheet.p); free(x.ac); return r; }
    {
        char t[160], l[4]; col_letter(NCOLS - 1, l);
        snprintf(t, sizeof t, "</sheetData><autoFilter ref=\"A1:%s%d\"/></worksheet>", l, x.row);
        bstr(&x.sheet, t);
    }

    /* лист "Борта" */
    qsort(x.ac, (size_t)x.nac, sizeof(AcRec), sort_ac);
    st->aircraft = x.nac;
    static const char *const ac_cols[] = {"Борт", "Тип самолёта", "Оператор", "Сообщений", "Первое сообщение", "Последнее сообщение",
                                           "Последний рейс", "Авиакомпания рейса", "Широта", "Долгота", "Позиция от", "Высота, м", "Высота, ft"};
    static const int ac_w[] = {12, 26, 26, 11, 19, 19, 11, 20, 10, 10, 19, 10, 10};
    Buf s2 = {0};
    sheet_head(&s2, 13, ac_w, 1);
    head_row(&s2, ac_cols, 13);
    for (int i = 0; i < x.nac; i++) {
        AcRec *a = &x.ac[i];
        int row = i + 2; char rr[24];
        snprintf(rr, sizeof rr, "<row r=\"%d\">", row); bstr(&s2, rr);
        cell_str(&s2, row, 0, a->reg, S_PLAIN);
        cell_str(&s2, row, 1, a->type, S_PLAIN);
        cell_str(&s2, row, 2, a->oper, S_PLAIN);
        cell_num(&s2, row, 3, a->msgs, S_INT, 0);
        int Y, M, D, h, mi, se;
        if (sscanf(a->first, "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &mi, &se) == 6) cell_num(&s2, row, 4, excel_serial(Y, M, D, h, mi, se), S_DATE, 8);
        if (sscanf(a->last, "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &mi, &se) == 6) cell_num(&s2, row, 5, excel_serial(Y, M, D, h, mi, se), S_DATE, 8);
        cell_str(&s2, row, 6, a->flight, S_PLAIN);
        cell_str(&s2, row, 7, a->airline, S_PLAIN);
        if (a->has_pos) {
            cell_num(&s2, row, 8, a->lat, S_COORD, 4); cell_num(&s2, row, 9, a->lon, S_COORD, 4);
            if (sscanf(a->pos_time, "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &mi, &se) == 6) cell_num(&s2, row, 10, excel_serial(Y, M, D, h, mi, se), S_DATE, 8);
        }
        if (a->has_alt) { cell_num(&s2, row, 11, floor(a->alt_ft * FEET_TO_METERS + 0.5), S_INT, 0); cell_num(&s2, row, 12, a->alt_ft, S_INT, 0); }
        bstr(&s2, "</row>");
    }
    {
        char t[128]; snprintf(t, sizeof t, "</sheetData><autoFilter ref=\"A1:M%d\"/></worksheet>", x.nac + 1);
        bstr(&s2, t);
    }

    /* лист "Пояснения" */
    Buf s3 = {0};
    static const int lw[] = {28, 120};
    sheet_head(&s3, 2, lw, 0);
    {
        static const char *const lc[] = {"Колонка", "Что это"};
        head_row(&s3, lc, 2);
        for (int i = 0; kLegend[i][0]; i++) {
            char rr[24]; snprintf(rr, sizeof rr, "<row r=\"%d\">", i + 2); bstr(&s3, rr);
            cell_str(&s3, i + 2, 0, kLegend[i][0], S_WRAP);
            cell_str(&s3, i + 2, 1, kLegend[i][1], S_WRAP);
            bstr(&s3, "</row>");
        }
    }
    bstr(&s3, "</sheetData></worksheet>");

    Buf wb = {0};
    {
        char t[256];
        bstr(&wb, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                  "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
                  "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
                  "<bookViews><workbookView xWindow=\"0\" yWindow=\"0\" windowWidth=\"28800\" windowHeight=\"15000\"/></bookViews><sheets>"
                  "<sheet name=\"Сообщения\" sheetId=\"1\" r:id=\"rId1\"/>"
                  "<sheet name=\"Борта\" sheetId=\"2\" r:id=\"rId2\"/>"
                  "<sheet name=\"Пояснения\" sheetId=\"3\" r:id=\"rId3\"/></sheets><definedNames>");
        char l[4]; col_letter(NCOLS - 1, l);
        snprintf(t, sizeof t, "<definedName name=\"_xlnm._FilterDatabase\" localSheetId=\"0\" hidden=\"1\">Сообщения!$A$1:$%s$%d</definedName>", l, x.row);
        bstr(&wb, t);
        snprintf(t, sizeof t, "<definedName name=\"_xlnm._FilterDatabase\" localSheetId=\"1\" hidden=\"1\">Борта!$A$1:$M$%d</definedName>", x.nac + 1);
        bstr(&wb, t);
        bstr(&wb, "</definedNames></workbook>");
    }
    static const char kCT[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
        "<Override PartName=\"/xl/worksheets/sheet2.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
        "<Override PartName=\"/xl/worksheets/sheet3.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
        "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/></Types>";
    static const char kRels[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/></Relationships>";
    static const char kWbRels[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet2.xml\"/>"
        "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet3.xml\"/>"
        "<Relationship Id=\"rId4\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/></Relationships>";

    ZEnt ents[10]; int ne = 0;
    zip_add(f, ents, &ne, "[Content_Types].xml", kCT, strlen(kCT));
    zip_add(f, ents, &ne, "_rels/.rels", kRels, strlen(kRels));
    zip_add(f, ents, &ne, "xl/workbook.xml", wb.p, wb.n);
    zip_add(f, ents, &ne, "xl/_rels/workbook.xml.rels", kWbRels, strlen(kWbRels));
    zip_add(f, ents, &ne, "xl/styles.xml", kStyles, strlen(kStyles));
    zip_add(f, ents, &ne, "xl/worksheets/sheet1.xml", x.sheet.p, x.sheet.n);
    zip_add(f, ents, &ne, "xl/worksheets/sheet2.xml", s2.p, s2.n);
    zip_add(f, ents, &ne, "xl/worksheets/sheet3.xml", s3.p, s3.n);
    zip_finish(f, ents, ne);
    int werr = ferror(f);
    fclose(f);
    free(x.sheet.p); free(x.ac); free(s2.p); free(s3.p); free(wb.p);
    return werr ? -2 : 0;
}


/* ================================================================== */
/* Отчёт по одному борту                                               */
/* ================================================================== */

typedef struct { ReportMsg *m; int n, cap; } RepCtx;

static int rep_cb(const char *line, const char *ts, const char *label, void *ctx) {
    RepCtx *c = (RepCtx *)ctx;
    static char text[8192], desc[1024];
    jx_get(line, "text", text, sizeof text);
    jx_get(line, "label_description", desc, sizeof desc);
    if (c->n >= c->cap) {
        int nc = c->cap ? c->cap * 2 : 256;
        ReportMsg *q = (ReportMsg *)realloc(c->m, (size_t)nc * sizeof *q);
        if (!q) return 0;
        c->m = q; c->cap = nc;
    }
    ReportMsg *m = &c->m[c->n++];
    m->time = strdup(ts);
    m->label = strdup(label);
    m->label_desc = strdup(desc);
    m->text = strdup(text);
    return 1;
}

char *journal_aircraft_report(const char *const *in, int n_in, const char *reg, const ExportFilter *flt, ExportStats *st) {
    ExportStats z = {0};
    if (!st) st = &z;
    memset(st, 0, sizeof *st);
    ExportFilter f = {0};
    if (flt) f = *flt;
    f.reg = reg;
    f.skip_service = 0;
    RepCtx c = {0};
    if (for_each_row(in, n_in, &f, st, rep_cb, &c) != 0) { free(c.m); return NULL; }
    /* журналы читаются по очереди (основной, потом запасной) - упорядочим по времени (стабильно) */
    for (int i = 1; i < c.n; i++) {
        ReportMsg t = c.m[i];
        int j = i - 1;
        while (j >= 0 && strcmp(c.m[j].time, t.time) > 0) { c.m[j + 1] = c.m[j]; j--; }
        c.m[j + 1] = t;
    }
    char info[400] = "";
    AircraftInfo ai;
    const char *r = reg;
    while (*r == '.') r++;
    if (aircraft_db_lookup(r, &ai))
        snprintf(info, sizeof info, "%.100s %.100s%s%.20s%s%s%.100s", ai.manufacturer, ai.model,
                 ai.typecode[0] ? " (" : "", ai.typecode, ai.typecode[0] ? ")" : "",
                 ai.operator_name[0] ? ", " : "", ai.operator_name);
    char *out = aircraft_report_build(r, info, f.from, f.to, c.m, c.n);
    for (int i = 0; i < c.n; i++) {
        free((char *)c.m[i].time); free((char *)c.m[i].label);
        free((char *)c.m[i].label_desc); free((char *)c.m[i].text);
    }
    free(c.m);
    return out;
}
