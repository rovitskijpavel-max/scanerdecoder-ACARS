#include "acars_report.h"
#include "airport_db.h"
#include "units.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* Справочники                                                         */
/* ------------------------------------------------------------------ */

static const struct { const char *code; const char *name; } kIata[] = {
    {"SU", "Аэрофлот"},        {"FV", "Россия (Rossiya)"}, {"U6", "Уральские авиалинии"},
    {"S7", "S7 Airlines"},     {"N4", "Nordwind"},         {"DP", "Победа"},
    {"UT", "ЮТэйр"},           {"WZ", "Red Wings"},        {"5N", "Smartavia"},
    {"A4", "Азимут"},          {"YC", "Ямал"},             {"DV", "SCAT Airlines"},
    {"GF", "Gulf Air"},        {"MU", "China Eastern"},    {"J2", "Azerbaijan Airlines"},
    {"PK", "Pakistan International"}, {"HY", "Uzbekistan Airways"},
    {"RJ", "Royal Jordanian"}, {"AA", "American Airlines"}, {"A9", "Georgian Airways"},
    {NULL, NULL}};

static const struct { const char *code; const char *name; } kIcao[] = {
    {"AFL", "Аэрофлот"},  {"SDM", "Россия (Rossiya)"}, {"SVR", "Уральские авиалинии"},
    {"SBI", "S7 Airlines"}, {"NWS", "Nordwind"},       {"PBD", "Победа"},
    {"UTA", "ЮТэйр"},     {"RWZ", "Red Wings"},        {NULL, NULL}};

const char *acars_airline_by_iata(const char *c) {
    if (!c || !c[0] || !c[1]) return NULL;
    for (int i = 0; kIata[i].code; i++)
        if (c[0] == kIata[i].code[0] && c[1] == kIata[i].code[1]) return kIata[i].name;
    return NULL;
}

const char *acars_airline_by_icao(const char *c) {
    if (!c || !c[0] || !c[1] || !c[2]) return NULL;
    for (int i = 0; kIcao[i].code; i++)
        if (strncmp(c, kIcao[i].code, 3) == 0) return kIcao[i].name;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Вспомогательное                                                     */
/* ------------------------------------------------------------------ */

typedef struct { char *buf; size_t cap, len; } Out;

static void add(Out *o, const char *fmt, ...) {
    if (o->len + 1 >= o->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= o->cap - o->len) o->len = o->cap - 1;
    else o->len += (size_t)n;
}

static int all_digits(const char *s, int n) {
    for (int i = 0; i < n; i++) if (!isdigit((unsigned char)s[i])) return 0;
    return 1;
}
static int all_char(const char *s, int n, char c) {
    for (int i = 0; i < n; i++) if (s[i] != c) return 0;
    return 1;
}

/* копия подстроки без пробелов/дефисов по краям */
static void sub(char *dst, size_t cap, const char *s, size_t n) {
    while (n && (*s == ' ' || *s == '\r' || *s == '\n')) { s++; n--; }
    while (n && (s[n-1] == ' ' || s[n-1] == '\r' || s[n-1] == '\n')) n--;
    if (n >= cap) n = cap - 1;
    memcpy(dst, s, n);
    dst[n] = 0;
}

static void airport_str(const char *icao, char *out, size_t cap) {
    AirportInfo ap;
    if (all_char(icao, 4, '-') || all_char(icao, 4, ' ')) { snprintf(out, cap, "нет данных"); return; }
    if (airport_db_lookup(icao, &ap) && ap.name[0])
        snprintf(out, cap, "%.4s - %.60s%s%.40s", icao, ap.name,
                 ap.municipality[0] ? ", " : "", ap.municipality);
    else
        snprintf(out, cap, "%.4s", icao);
}

static void hhmm(const char *s, char *out, size_t cap) { snprintf(out, cap, "%.2s:%.2s", s, s + 2); }
static void hhmmss(const char *s, char *out, size_t cap) { snprintf(out, cap, "%.2s:%.2s:%.2s", s, s + 2, s + 4); }

/* ------------------------------------------------------------------ */
/* Заголовок: MSN + рейс                                               */
/* ------------------------------------------------------------------ */

int acars_header_parse(const char *t, AcarsHeader *h) {
    memset(h, 0, sizeof(*h));
    if (!t || strlen(t) < 10) return 0;
    if (!isupper((unsigned char)t[0]) || !isdigit((unsigned char)t[1]) ||
        !isdigit((unsigned char)t[2]) || !isupper((unsigned char)t[3]))
        return 0;
    /* рейс: 2 символа кода (буква/цифра, хотя бы одна буква) + 4 символа
     * номера (цифры, буквы или пробелы) */
    if (!isalnum((unsigned char)t[4]) || !isalnum((unsigned char)t[5])) return 0;
    if (!isalpha((unsigned char)t[4]) && !isalpha((unsigned char)t[5])) return 0;
    for (int i = 6; i < 10; i++)
        if (!isalnum((unsigned char)t[i]) && t[i] != ' ') return 0;
    memcpy(h->msn, t, 4); h->msn[4] = 0;
    h->seq = (t[1] - '0') * 10 + (t[2] - '0');
    sub(h->flight, sizeof(h->flight), t + 4, 6);
    h->valid = 1;
    return 1;
}

static void flight_line(Out *o, const AcarsHeader *h) {
    const char *al = acars_airline_by_iata(h->flight);
    const char *num = h->flight + 2;
    while (*num == '0' && num[1]) num++;
    add(o, "  Рейс: %s", h->flight);
    if (strcmp(num, "0") == 0 || strcmp(num, "000") == 0)
        add(o, " - номер рейса не задан (нули: экипаж/система ещё не ввели рейс)");
    else if (al)
        add(o, " - %s, рейс №%s", al, num);
    add(o, "\n");
}

/* ------------------------------------------------------------------ */
/* Строки тела ASU/AFL-отчёта                                          */
/* ------------------------------------------------------------------ */

static void fuel_line(Out *o, const char *raw, const char *val) {
    if (!val[0] || all_char(val, (int)strlen(val), '-')) {
        add(o, "  \"%s\" -> остаток топлива: нет данных\n", raw);
        return;
    }
    int n = atoi(val);
    add(o, "  \"%s\" -> остаток топлива на борту: %d  (вероятно, сотни кг, т.е. ~%d.%d т - "
           "единицы не подтверждены документацией)\n", raw, n, n / 10, n % 10);
}

static void body_line(Out *o, const char *ln, size_t len) {
    char raw[80]; sub(raw, sizeof raw, ln, len);
    if (!raw[0]) return;
    char val[48];
    #define KEY(k) (len >= strlen(k) && strncmp(ln, k, strlen(k)) == 0)
    #define VAL(k) sub(val, sizeof val, ln + strlen(k), len - strlen(k))

    if (len == 6 && all_digits(ln, 6)) {
        char t[16]; hhmmss(ln, t, sizeof t);
        add(o, "  \"%s\" -> время события: %s UTC (сообщение могло быть отправлено позже самого события)\n", raw, t);
    } else if (len == 12 && all_char(ln, 6, '-') && all_digits(ln + 6, 6)) {
        char t[16]; hhmmss(ln + 6, t, sizeof t);
        add(o, "  \"%s\" -> время события: %s UTC (первая часть - прочерки: поле не заполнено)\n", raw, t);
    } else if (KEY("FOB")) {
        VAL("FOB"); fuel_line(o, raw, val);
    } else if (KEY("FUEL")) {
        VAL("FUEL"); fuel_line(o, raw, val);
    } else if (KEY("TEMP")) {
        VAL("TEMP"); char c[48]; int j = 0;
        for (char *q = val; *q; q++) { if (*q != ' ') c[j++] = *q; }
        c[j] = 0;
        add(o, "  \"%s\" -> температура за бортом: %s%s\n", raw, c[0] ? c : "нет данных", c[0] ? " °C" : "");
    } else if (KEY("WDIR")) {
        VAL("WDIR"); char c[48]; int j = 0;
        for (char *q = val; *q; q++) { if (*q != ' ') c[j++] = *q; }
        c[j] = 0;
        if (c[0] && all_digits(c, j)) {
            char w[160]; wind_text(c, "", w, sizeof w);
            char *cut = strstr(w, ", скорость"); if (cut) *cut = 0;
            add(o, "  \"%s\" -> направление ветра: %s. Это направление, ОТКУДА дует ветер, отсчёт от севера по часовой "
                   "стрелке (так принято в авиации); число читается как сотые доли градуса (все значения в журнале "
                   "укладываются в 0-359.99). Формат документацией не подтверждён\n", raw, w);
        } else add(o, "  \"%s\" -> направление ветра: %s\n", raw, c[0] ? c : "нет данных");
    } else if (KEY("WSPD")) {
        VAL("WSPD");
        char c[48]; int j = 0;
        for (char *q = val; *q; q++) { if (*q != ' ') c[j++] = *q; }
        c[j] = 0;
        if (c[0] && all_digits(c, j)) {
            char w[160]; wind_text("", c, w, sizeof w);
            const char *sp = strstr(w, ", ");
            add(o, "  \"%s\" -> скорость ветра: %s. Вероятно, узлы (скорость растёт с высотой: 5 у земли, 75-85 на 35000 ft - "
                   "похоже на узлы; не подтверждено)\n", raw, sp ? sp + 2 : w);
        } else add(o, "  \"%s\" -> скорость ветра: %s\n", raw, c[0] ? c : "нет данных");
    } else if (KEY("LATN") || KEY("LATS")) {
        VAL("LATN");
        add(o, "  \"%s\" -> широта: %s° %s\n", raw, val, ln[3] == 'N' ? "с.ш." : "ю.ш.");
    } else if (KEY("LONE") || KEY("LONW")) {
        VAL("LONE");
        add(o, "  \"%s\" -> долгота: %s° %s\n", raw, val, ln[3] == 'E' ? "в.д." : "з.д.");
    } else if (KEY("ETA") && len >= 7 && all_digits(ln + 3, 4)) {
        char t[16]; hhmm(ln + 3, t, sizeof t);
        add(o, "  \"%s\" -> расчётное время прибытия: %s UTC\n", raw, t);
    } else if (KEY("TUR")) {
        VAL("TUR");
        add(o, "  \"%s\" -> турбулентность: %s\n", raw, val[0] ? val : "не передана (поле пустое)");
    } else if (KEY("ALT") && len > 3 && (ln[3] == ' ' || isdigit((unsigned char)ln[3]))) {
        VAL("ALT");
        char at[48]; alt_ft_text(val, at, sizeof at, NULL);
        add(o, "  \"%s\" -> высота: %s (передана в футах - предположительно)\n", raw, at);
    } else if (KEY("DEPS") || KEY("DEST")) {
        char ap[160]; airport_str(ln + 4, ap, sizeof ap);
        add(o, "  \"%s\" -> %s: %s\n", raw, KEY("DEPS") ? "аэропорт вылета" : "аэропорт назначения", ap);
    } else if ((KEY("OUTT") || KEY("OFFT") || KEY("ONNT") || KEY("INNT")) && len >= 10 && all_digits(ln + 4, 6)) {
        const char *nm = KEY("OUTT") ? "OUT - выруливание/уход от гейта" :
                         KEY("OFFT") ? "OFF - взлёт" : KEY("ONNT") ? "ON - посадка" : "IN - прибытие на гейт";
        char t[16]; hhmmss(ln + 4, t, sizeof t);
        add(o, "  \"%s\" -> %s: %s UTC\n", raw, nm, t);
    } else if ((KEY("OIBT") || KEY("FLTT")) && len >= 10 && all_digits(ln + 4, 6)) {
        char t[16]; hhmmss(ln + 4, t, sizeof t);
        add(o, "  \"%s\" -> %s: %s (по значению совпадает с интервалом %s)\n", raw,
            KEY("OIBT") ? "время в блоках" : "время в полёте", t,
            KEY("OIBT") ? "OUT-IN" : "OFF-ON");
    } else {
        add(o, "  \"%s\" -> поле фирменного формата, значение не расшифровано (в публичных "
               "источниках не описано)\n", raw);
    }
    #undef KEY
    #undef VAL
}

static const char *report_type_desc(const char *t, int *sure) {
    static const struct { const char *c, *d; int sure; } tab[] = {
        {"POS", "позиционный отчёт (текущее положение борта)", 1},
        {"ETA", "отчёт о расчётном времени прибытия + положение борта", 1},
        {"ALT", "отчёт ALT (содержит высоту; точное назначение типа не подтверждено)", 0},
        {"OFF", "взлёт (колёса оторвались от ВПП)", 1},
        {"INN", "прибытие на гейт (IN)", 1},
        {"SFI", "итоговая сводка по рейсу (времена OUT/OFF/ON/IN, топливо)", 1},
        {"PBS", "вероятно: начало буксировки от гейта (pushback) - по времени между закрытием дверей и взлётом", 0},
        {"ADC", "вероятно: все двери закрыты - по порядку событий перед вылетом", 0},
        {"LDC", "вероятно: двери закрыты - по аббревиатуре, не подтверждено", 0},
        {"LDO", "вероятно: двери открыты после прибытия - по порядку событий", 0},
        {"APS", "сводка по двигателям/ВСУ (поля APUS/ENGS/AEON/AEOF) - точный смысл не подтверждён", 0},
        {"RTG", "назначение типа не подтверждено", 0},
        {NULL, NULL, 0}};
    for (int i = 0; tab[i].c; i++)
        if (strncmp(t, tab[i].c, 3) == 0) { *sure = tab[i].sure; return tab[i].d; }
    *sure = 0;
    return "неизвестный тип отчёта";
}

/* первая строка "POS01AFL1603   /29151006UWWWUUEE" */
static int asu_first_line(Out *o, const char *r) {
    if (strlen(r) < 16) return 0;
    for (int i = 0; i < 3; i++) if (!isupper((unsigned char)r[i])) return 0;
    if (!isdigit((unsigned char)r[3]) || !isdigit((unsigned char)r[4]) || r[15] != '/') return 0;
    int sure; char tt[4]; memcpy(tt, r, 3); tt[3] = 0;
    add(o, "  Тип отчёта: %s - %s\n", tt, report_type_desc(tt, &sure));
    char cs[16]; sub(cs, sizeof cs, r + 5, 10);
    if (cs[0] && !all_char(cs, (int)strlen(cs), '-')) {
        const char *al = acars_airline_by_icao(cs);
        add(o, "  Позывной: %s%s%s\n", cs, al ? " - " : "", al ? al : "");
    } else add(o, "  Позывной: не передан (поле заполнено прочерками)\n");
    size_t L = strlen(r);
    if (L >= 24) {
        const char *g = r + 16;
        if (all_digits(g, 8)) {
            char t[16]; hhmm(g + 4, t, sizeof t);
            add(o, "  Время группы: %.2s-е число, %s UTC", g, t);
            if (strncmp(g, g + 2, 2) != 0)
                add(o, " (между числом и временем стоит пара \"%.2s\" - назначение не определено)", g + 2);
            add(o, "\n");
        } else {
            char raw[16]; sub(raw, sizeof raw, g, 8);
            add(o, "  Группа даты/времени: \"%s\" - формат отличается от обычного, не расшифрована\n", raw);
        }
    }
    if (L >= 32) {
        char a[160];
        airport_str(r + 24, a, sizeof a); add(o, "  Откуда: %s\n", a);
        airport_str(r + 28, a, sizeof a); add(o, "  Куда:   %s\n", a);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Остальные форматы                                                   */
/* ------------------------------------------------------------------ */

static void decode_position_csv(Out *o, const char *r) {
    /* "//104204,28002,1112,----.-,MMMM.MMM MMMM.MMM," или "102019,34000,1110, 106,N 58.108 E 42.206" */
    while (*r == '/') r++;
    char f[6][48]; int n = 0; const char *p = r;
    while (n < 6) {
        const char *c = strchr(p, ',');
        size_t l = c ? (size_t)(c - p) : strlen(p);
        sub(f[n], sizeof f[n], p, l); n++;
        if (!c) break;
        p = c + 1;
    }
    if (n < 5 || strlen(f[0]) != 6 || !all_digits(f[0], 6)) {
        add(o, "  Содержимое: %s\n  (формат с запятыми не распознан)\n", r);
        return;
    }
    char t[16]; hhmmss(f[0], t, sizeof t);
    add(o, "  Позиционный отчёт (поля через запятую):\n");
    add(o, "    \"%s\" -> время: %s UTC\n", f[0], t);
    if (f[1][0] && all_digits(f[1], (int)strlen(f[1])))
    {
        char at[48]; alt_ft_text(f[1], at, sizeof at, NULL);
        add(o, "    \"%s\" -> высота: %s, эшелон FL%d (передана в футах - предположительно)\n", f[1], at, atoi(f[1]) / 100);
    }
    else add(o, "    \"%s\" -> высота: нет данных\n", f[1]);
    if (strlen(f[2]) == 4 && all_digits(f[2], 4)) { hhmm(f[2], t, sizeof t); add(o, "    \"%s\" -> расчётное время прибытия: %s UTC\n", f[2], t); }
    else add(o, "    \"%s\" -> время прибытия: нет данных\n", f[2]);
    add(o, "    \"%s\" -> число, назначение не подтверждено (возможно, остаток топлива)\n", f[3]);
    if (strchr(f[4], 'M') && strstr(f[4], "MMMM"))
        add(o, "    \"%s\" -> координаты не получены (заполнитель MMMM = нет данных навигации)\n", f[4]);
    else
        add(o, "    \"%s\" -> координаты (широта N/S, долгота E/W)\n", f[4]);
}

static void decode_short(Out *o, const char *label, const char *r) {
    char rest[64]; sub(rest, sizeof rest, r, strlen(r));
    if (strcmp(label, "Q0") == 0)
        add(o, "  Проверка канала (link test): борт сообщает, что связь работает; полезных данных нет\n");
    else if (strcmp(label, "5P") == 0)
        add(o, "  Служебная метка 5P (\"временная приостановка\" по справочнику) - данных нет\n");
    else if (strcmp(label, "Q6") == 0)
        add(o, "  Q6 - переход с голосовой связи на ACARS - данных нет\n");
    else if (strcmp(label, "5V") == 0)
        add(o, "  Метка 5V - назначение не описано ни в одном публичном источнике\n");
    if (strcmp(label, "SA") == 0 && strlen(rest) == 10 && rest[0] == '0' &&
        isupper((unsigned char)rest[1]) && isupper((unsigned char)rest[2]) &&
        all_digits(rest + 3, 6) && rest[9] == 'V') {
        char t[16]; hhmmss(rest + 3, t, sizeof t);
        add(o, "  Служебное сообщение SA: код \"%.2s\", время %s UTC, признак \"V\".\n"
               "  Смысл кода %.2s в публичных источниках не найден (встречались EV и LH) - "
               "время подтверждено по моменту приёма.\n", rest + 1, t, rest + 1);
    } else if (rest[0]) {
        add(o, "  Содержимое после номера рейса: \"%s\" (не расшифровано)\n", rest);
    }
}

static int decode_atis_header(Out *o, const char *t) {
    /* "/LEMZTST.TI2/UUWW ARR ATIS D" - адрес.формат/текст */
    const char *dot = strchr(t, '.'), *s2 = dot ? strchr(dot, '/') : NULL;
    if (t[0] != '/' || !dot || !s2 || dot - t > 12 || s2 - dot > 6) return 0;
    char a[16], f[8];
    sub(a, sizeof a, t + 1, (size_t)(dot - t - 1));
    sub(f, sizeof f, dot + 1, (size_t)(s2 - dot - 1));
    add(o, "  Адрес наземной системы: %s, формат/тип данных (IMI): %s - значение кода %s не найдено в "
           "публичных источниках\n", a, f, f);
    return (int)(s2 - t) + 1;
}

static void decode_fsm(Out *o, const char *p) {
    /* "FSM 1037 260929 ULLI" */
    if (strncmp(p, "FSM ", 4) != 0 || strlen(p) < 20) return;
    char hh[8], dd[16], ap[160];
    if (!all_digits(p + 4, 4) || p[8] != ' ' || !all_digits(p + 9, 6)) return;
    snprintf(hh, sizeof hh, "%.2s:%.2s", p + 4, p + 6);
    snprintf(dd, sizeof dd, "20%.2s-%.2s-%.2s", p + 9, p + 11, p + 13);
    airport_str(p + 16, ap, sizeof ap);
    add(o, "  FSM (сообщение службы): %s UTC, дата %s, аэропорт %s\n", hh, dd, ap);
}

static void decode_atis_text(Out *o, const char *p) {
    /* ищем "ARR ATIS", "DEP ATIS", "ENR ATIS" с кодом аэропорта перед ними */
    static const struct { const char *k, *d; } kinds[] = {
        {"ARR ATIS", "ATIS на прибытие"}, {"DEP ATIS", "ATIS на вылет"}, {"ENR ATIS", "ATIS по маршруту (en route)"}};
    for (int i = 0; i < 3; i++) {
        const char *q = strstr(p, kinds[i].k);
        if (q && q - p >= 5 && isupper((unsigned char)q[-5]) && q[-1] == ' ') {
            char ap[160]; airport_str(q - 5, ap, sizeof ap);
            add(o, "  %s: %s", kinds[i].d, ap);
            if (q[8] == ' ' && isupper((unsigned char)q[9]) && !isalnum((unsigned char)q[10]))
                add(o, ", информационная литера %c", q[9]);
            add(o, "\n");
            return;
        }
    }
    if (strstr(p, "DEMAND") && strstr(p, "SIGMET"))
        add(o, "  Сводка по запросу (DEMAND): SIGMET/METAR по перечисленным аэродромам (METAR ниже\n"
               "  расшифровывается отдельным блоком)\n");
}

/* Заголовок Type B ".DMEASS7 291026 / AGM / AN RA73335/MA 584A" */
static void decode_typeb(Out *o, const char *t) {
    if (t[0] != '.') return;
    const char *e = strpbrk(t, " \r\n");
    if (!e || e - t < 3 || e - t > 10) return;
    char a[16]; sub(a, sizeof a, t + 1, (size_t)(e - t - 1));
    add(o, "  Адрес отправителя (Type B): %s\n", a);
    if (e[0] == ' ' && all_digits(e + 1, 6))
        add(o, "  Время отправки: %.2s-е число, %.2s:%.2s UTC\n", e + 1, e + 3, e + 5);
    const char *an = strstr(t, "AN ");
    if (an) {
        const char *end = an + 3; while (isalnum((unsigned char)*end) || *end == '-') end++;
        char reg[16]; sub(reg, sizeof reg, an + 3, (size_t)(end - an - 3));
        add(o, "  Борт получателя (AN): %s\n", reg);
        if (strncmp(end, "/MA ", 4) == 0) {
            char ma[12]; const char *m = end + 4, *me = m; while (isalnum((unsigned char)*me)) me++;
            sub(ma, sizeof ma, m, (size_t)(me - m));
            add(o, "  Номер подтверждения доставки (MA): %s\n", ma);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Точка входа                                                         */
/* ------------------------------------------------------------------ */

int acars_report_decode(const char *label, const char *text, char *out, size_t out_size) {
    if (out_size) out[0] = 0;
    if (!text || !label || !out_size) return 0;
    Out o = {out, out_size, 0};

    if (strcmp(label, "SQ") == 0 && !text[0]) return 0;
    if (strcmp(label, "SQ") == 0) {
        add(&o, "  Служебный сигнал наземной станции (squitter, ARINC 618): станция сообщает о себе\n"
                "  всем бортам в зоне. Содержимое \"%s\" - внутренний формат, не расшифровывается.\n", text);
        return o.len > 0;
    }

    AcarsHeader h;
    if (acars_header_parse(text, &h) && text[0] != '/' && text[0] != '.') {
        add(&o, "  Номер сообщения (MSN): %s - порядковый номер %d (формат ARINC 620: буква + 2 цифры + буква)\n", h.msn, h.seq);
        flight_line(&o, &h);
        const char *rest = text + 10;

        if (strlen(rest) >= 16 && rest[15] == '/' && asu_first_line(&o, rest)) {
            const char *nl = strpbrk(rest, "\r\n");
            if (nl) {
                add(&o, "  Данные:\n");
                const char *p = nl;
                while (*p) {
                    while (*p == '\r' || *p == '\n') p++;
                    size_t l = strcspn(p, "\r\n");
                    if (l) body_line(&o, p, l);
                    p += l;
                }
            }
        } else if (strcmp(label, "16") == 0 && strchr(rest, ',')) {
            decode_position_csv(&o, rest);
        } else if (strlen(rest) >= 7 && isupper((unsigned char)rest[0]) && rest[4] == '-' &&
                   (strncmp(rest + 5, "SA", 2) == 0 || strncmp(rest + 5, "FT", 2) == 0) &&
                   isupper((unsigned char)rest[3])) {
            char ap[160]; airport_str(rest, ap, sizeof ap);
            add(&o, "  Запрос погоды по аэропорту %s: тип %s\n", ap,
                rest[5] == 'S' ? "SA - METAR (фактическая погода)" : "FT - TAF (прогноз)");
        } else if (strncmp(rest, "#", 1) == 0) {
            /* H1 с #DFB/#CFB: расшифровкой занимаются cfdiu_fault.c / справка меток */
        } else if (rest[0] == '/') {
            if (!decode_atis_header(&o, rest)) {
                char r[96]; sub(r, sizeof r, rest, strlen(rest));
                add(&o, "  Содержимое: \"%s\" (адресная строка авиакомпании, не расшифрована)\n", r);
            }
            if (strcmp(label, "B9") == 0)
                add(&o, "  Запрос ATIS: аэродром в адресе, в хвосте - служебные символы (вероятно, контрольная сумма)\n");
        } else if (label[0] == 'Q' || label[0] == '5' || strcmp(label, "SA") == 0) {
            decode_short(&o, label, rest);
        } else if (rest[0]) {
            char r[64]; sub(r, sizeof r, rest, strlen(rest));
            if (r[0]) add(&o, "  Содержимое: \"%s\" (формат авиакомпании, не расшифровано)\n", r);
        }
        return o.len > 0;
    }

    /* Уплинки без MSN-заголовка */
    int skip = decode_atis_header(&o, text);
    const char *body = text + skip;
    decode_fsm(&o, body + strspn(body, "\r\n"));
    if (strcmp(label, "A4") == 0 && strstr(text, "REJECTED"))
        add(&o, "  Диспетчерская служба ОТКЛОНИЛА запрос борта (REJECTED) - экипажу предписано\n"
                "  перейти на голосовую связь (REVERT TO VOICE PROCEDURES)\n");
    if (strcmp(label, "A9") == 0 || strcmp(label, "B9") == 0) decode_atis_text(&o, body);
    decode_typeb(&o, text);
    return o.len > 0;
}

/* ------------------------------------------------------------------ */
/* Структурированная сводка (для Excel)                                */
/* ------------------------------------------------------------------ */

static void squeeze(char *dst, size_t cap, const char *src, size_t n) {
    size_t j = 0;
    for (size_t i = 0; i < n && j + 1 < cap; i++)
        if (src[i] != ' ' && src[i] != '\r' && src[i] != '\n') dst[j++] = src[i];
    dst[j] = 0;
}

int acars_report_summary(const char *label, const char *text, AcarsSummary *s) {
    memset(s, 0, sizeof(*s));
    if (!label || !text) return 0;
    AcarsHeader h;
    if (!acars_header_parse(text, &h) || text[0] == '/' || text[0] == '.') return 0;
    snprintf(s->flight, sizeof s->flight, "%s", h.flight);
    const char *al = acars_airline_by_iata(h.flight);
    if (al) snprintf(s->airline, sizeof s->airline, "%s", al);
    s->valid = 1;
    const char *rest = text + 10;
    if (strlen(rest) >= 16 && rest[15] == '/' && isupper((unsigned char)rest[0]) &&
        isupper((unsigned char)rest[1]) && isupper((unsigned char)rest[2]) &&
        isdigit((unsigned char)rest[3]) && isdigit((unsigned char)rest[4])) {
        memcpy(s->type, rest, 3);
        char cs[16]; sub(cs, sizeof cs, rest + 5, 10);
        if (!all_char(cs, (int)strlen(cs), '-')) snprintf(s->callsign, sizeof s->callsign, "%s", cs);
        if (strlen(rest) >= 32) {
            if (!all_char(rest + 24, 4, '-') && !all_char(rest + 24, 4, ' ')) memcpy(s->dep, rest + 24, 4);
            if (!all_char(rest + 28, 4, '-') && !all_char(rest + 28, 4, ' ')) memcpy(s->dest, rest + 28, 4);
        }
        const char *nl = strpbrk(rest, "\r\n");
        const char *p = nl ? nl : rest + strlen(rest);
        char wdir[16] = "", wspd[16] = "";
        while (*p) {
            while (*p == '\r' || *p == '\n') p++;
            size_t l = strcspn(p, "\r\n");
            if (l == 6 && all_digits(p, 6)) hhmmss(p, s->event_time, sizeof s->event_time);
            else if (l == 12 && all_char(p, 6, '-') && all_digits(p + 6, 6)) hhmmss(p + 6, s->event_time, sizeof s->event_time);
            else if (l > 3 && strncmp(p, "FOB", 3) == 0) squeeze(s->fuel, sizeof s->fuel, p + 3, l - 3);
            else if (l > 4 && strncmp(p, "FUEL", 4) == 0) squeeze(s->fuel, sizeof s->fuel, p + 4, l - 4);
            else if (l > 4 && strncmp(p, "TEMP", 4) == 0) squeeze(s->temp, sizeof s->temp, p + 4, l - 4);
            else if (l > 4 && strncmp(p, "WDIR", 4) == 0) squeeze(wdir, sizeof wdir, p + 4, l - 4);
            else if (l > 4 && strncmp(p, "WSPD", 4) == 0) squeeze(wspd, sizeof wspd, p + 4, l - 4);
            else if (l >= 7 && strncmp(p, "ETA", 3) == 0 && all_digits(p + 3, 4)) hhmm(p + 3, s->eta, sizeof s->eta);
            else if (l > 3 && strncmp(p, "ALT", 3) == 0 && (p[3] == ' ' || isdigit((unsigned char)p[3]))) {
                char v[16]; squeeze(v, sizeof v, p + 3, l - 3);
                if (v[0] && all_digits(v, (int)strlen(v))) { s->has_alt = 1; s->alt_ft = atoi(v); }
            } else if (l > 4 && (strncmp(p, "LATN", 4) == 0 || strncmp(p, "LATS", 4) == 0)) {
                char v[24]; squeeze(v, sizeof v, p + 4, l - 4);
                s->lat = atof(v) * (p[3] == 'S' ? -1 : 1); s->has_pos |= 1;
            } else if (l > 4 && (strncmp(p, "LONE", 4) == 0 || strncmp(p, "LONW", 4) == 0)) {
                char v[24]; squeeze(v, sizeof v, p + 4, l - 4);
                s->lon = atof(v) * (p[3] == 'W' ? -1 : 1); s->has_pos |= 2;
            }
            p += l;
        }
        s->has_pos = (s->has_pos == 3);
        if (wdir[0] || wspd[0])
            wind_text(wdir, wspd, s->wind, sizeof s->wind);
    } else if (strcmp(label, "16") == 0 && strchr(rest, ',')) {
        while (*rest == '/') rest++;
        char f[6][48]; int n = 0; const char *p = rest;
        while (n < 6) {
            const char *c = strchr(p, ',');
            size_t l = c ? (size_t)(c - p) : strlen(p);
            sub(f[n], sizeof f[n], p, l); n++;
            if (!c) break;
            p = c + 1;
        }
        if (n >= 5 && strlen(f[0]) == 6 && all_digits(f[0], 6)) {
            hhmmss(f[0], s->event_time, sizeof s->event_time);
            if (f[1][0] && all_digits(f[1], (int)strlen(f[1]))) { s->has_alt = 1; s->alt_ft = atoi(f[1]); }
            if (strlen(f[2]) == 4 && all_digits(f[2], 4)) hhmm(f[2], s->eta, sizeof s->eta);
            char la, lo; double a, b;
            if (sscanf(f[4], "%c %lf %c %lf", &la, &a, &lo, &b) == 4 &&
                (la == 'N' || la == 'S') && (lo == 'E' || lo == 'W')) {
                s->lat = la == 'S' ? -a : a; s->lon = lo == 'W' ? -b : b; s->has_pos = 1;
            }
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Короткие строки для карточки точки на карте                         */
/* ------------------------------------------------------------------ */

static void point_line(Out *o, int *n, const char *name, const char *val) {
    add(o, "%s: %s\n", name, val);
    (*n)++;
}

int acars_report_point(const char *label, const char *text, char *out, size_t out_size) {
    if (out_size) out[0] = 0;
    if (!label || !text || !out_size) return 0;
    Out o = {out, out_size, 0};
    int n = 0;
    AcarsHeader h;
    if (!acars_header_parse(text, &h) || text[0] == '/' || text[0] == '.') return 0;
    char v[160];
    const char *al = acars_airline_by_iata(h.flight);
    snprintf(v, sizeof v, "%s%s%s", h.flight, al ? " - " : "", al ? al : "");
    point_line(&o, &n, "Рейс", v);
    snprintf(v, sizeof v, "%s (№%d)", h.msn, h.seq);
    point_line(&o, &n, "Номер сообщения", v);
    const char *rest = text + 10;

    if (strlen(rest) >= 16 && rest[15] == '/' && isupper((unsigned char)rest[0]) && isdigit((unsigned char)rest[3])) {
        int sure; char tt[4]; memcpy(tt, rest, 3); tt[3] = 0;
        snprintf(v, sizeof v, "%s - %s", tt, report_type_desc(tt, &sure));
        point_line(&o, &n, "Тип отчёта", v);
        char cs[16]; sub(cs, sizeof cs, rest + 5, 10);
        if (cs[0] && !all_char(cs, (int)strlen(cs), '-')) point_line(&o, &n, "Позывной", cs);
        if (strlen(rest) >= 32) {
            char a[100], b[100], ra[240];
            if (!all_char(rest + 24, 4, '-')) { airport_str(rest + 24, a, sizeof a); airport_str(rest + 28, b, sizeof b);
                snprintf(ra, sizeof ra, "%s  ->  %s", a, b); point_line(&o, &n, "Маршрут", ra); }
        }
        const char *nl = strpbrk(rest, "\r\n");
        const char *p = nl ? nl : rest + strlen(rest);
        char wdir[16] = "", wspd[16] = "";
        int have_w = 0;
        while (*p) {
            while (*p == '\r' || *p == '\n') p++;
            size_t l = strcspn(p, "\r\n");
            char val[64]; val[0] = 0;
            if (l == 0) { continue; }
            if (l == 6 && all_digits(p, 6)) { hhmmss(p, val, sizeof val); strcat(val, " UTC"); point_line(&o, &n, "Время события", val); }
            else if (l == 12 && all_char(p, 6, '-') && all_digits(p + 6, 6)) { hhmmss(p + 6, val, sizeof val); strcat(val, " UTC"); point_line(&o, &n, "Время события", val); }
            else if ((l >= 3 && strncmp(p, "FOB", 3) == 0) || (l >= 4 && strncmp(p, "FUEL", 4) == 0)) {
                size_t k = p[0] == 'F' && p[1] == 'O' ? 3 : 4;
                char t[24]; squeeze(t, sizeof t, p + k, l - k);
                if (!t[0] || all_char(t, (int)strlen(t), '-')) snprintf(v, sizeof v, "нет данных");
                else { int f = atoi(t); snprintf(v, sizeof v, "%d  (вероятно, сотни кг: ~%d.%d т)", f, f / 10, f % 10); }
                point_line(&o, &n, "Топливо", v);
            } else if (l >= 4 && strncmp(p, "TEMP", 4) == 0) {
                char t[24]; squeeze(t, sizeof t, p + 4, l - 4);
                snprintf(v, sizeof v, t[0] ? "%s °C" : "нет данных", t);
                point_line(&o, &n, "Температура за бортом", v);
            } else if (l >= 4 && strncmp(p, "WDIR", 4) == 0) { squeeze(wdir, sizeof wdir, p + 4, l - 4); have_w = 1; }
            else if (l >= 4 && strncmp(p, "WSPD", 4) == 0) { squeeze(wspd, sizeof wspd, p + 4, l - 4); have_w = 1; }
            else if (l >= 7 && strncmp(p, "ETA", 3) == 0 && all_digits(p + 3, 4)) { hhmm(p + 3, val, sizeof val); strcat(val, " UTC"); point_line(&o, &n, "Расчётное прибытие (ETA)", val); }
            else if (l >= 3 && strncmp(p, "TUR", 3) == 0) { char t[24]; squeeze(t, sizeof t, p + 3, l - 3);
                point_line(&o, &n, "Турбулентность", t[0] ? t : "не передана"); }
            else if (l > 3 && strncmp(p, "ALT", 3) == 0 && (p[3] == ' ' || isdigit((unsigned char)p[3]))) {
                char t[24]; squeeze(t, sizeof t, p + 3, l - 3); alt_ft_text(t, v, sizeof v, NULL);
                point_line(&o, &n, "Высота", v);
            } else if (l >= 4 && (strncmp(p, "LATN", 4) == 0 || strncmp(p, "LATS", 4) == 0 || strncmp(p, "LONE", 4) == 0 || strncmp(p, "LONW", 4) == 0)) {
                /* координаты показываются отдельной строкой карточки */
            } else {
                char raw[80]; sub(raw, sizeof raw, p, l);
                if (raw[0]) point_line(&o, &n, "Поле", raw);
            }
            p += l;
        }
        if (have_w) {
            wind_text(wdir, wspd, v, sizeof v);
            point_line(&o, &n, "Ветер", v);
        }
        return n;
    }
    if (strcmp(label, "16") == 0 && strchr(rest, ',')) {
        AcarsSummary sm;
        acars_report_summary(label, text, &sm);
        if (sm.event_time[0]) { snprintf(v, sizeof v, "%s UTC", sm.event_time); point_line(&o, &n, "Время", v); }
        if (sm.has_alt) { char t[24]; snprintf(t, sizeof t, "%d", sm.alt_ft); alt_ft_text(t, v, sizeof v, NULL); point_line(&o, &n, "Высота", v); }
        if (sm.eta[0]) { snprintf(v, sizeof v, "%s UTC", sm.eta); point_line(&o, &n, "Расчётное прибытие (ETA)", v); }
        return n;
    }
    return n;
}
