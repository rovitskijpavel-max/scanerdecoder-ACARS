#include "aircraft_report.h"
#include "acars_report.h"
#include "airport_db.h"
#include "geomap.h"
#include "units.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int aircraft_report_flights(const ReportMsg *m, int n, ReportFlight *out, int max) {
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        AcarsSummary s;
        if (!acars_report_summary(m[i].label, m[i].text, &s) || (!s.dep[0] && !s.dest[0] && !s.flight[0]))
            continue;
        int k = -1;
        for (int j = 0; j < cnt; j++)
            if (!strcmp(out[j].flight, s.flight) && !strcmp(out[j].dep, s.dep) && !strcmp(out[j].dest, s.dest)) {
                k = j;
                break;
            }
        if (k < 0) {
            if (cnt >= max)
                continue;
            k = cnt++;
            memset(&out[k], 0, sizeof out[k]);
            snprintf(out[k].flight, sizeof out[k].flight, "%s", s.flight);
            snprintf(out[k].dep, sizeof out[k].dep, "%s", s.dep);
            snprintf(out[k].dest, sizeof out[k].dest, "%s", s.dest);
        }
        out[k].count++;
        snprintf(out[k].time, sizeof out[k].time, "%.19s", m[i].time);
    }
    return cnt;
}

int aircraft_report_last_route(const ReportMsg *m, int n, char *flight8, char *dep5, char *dest5) {
    int found = 0;
    flight8[0] = dep5[0] = dest5[0] = '\0';
    for (int i = n - 1; i >= 0; i--) {
        AcarsSummary s;
        if (!acars_report_summary(m[i].label, m[i].text, &s))
            continue;
        if (!flight8[0] && s.flight[0]) { snprintf(flight8, 8, "%s", s.flight); found = 1; }
        if (!dep5[0] && s.dep[0] && s.dest[0]) {
            /* вылет и назначение берём парой из ОДНОГО сообщения */
            snprintf(dep5, 5, "%s", s.dep);
            snprintf(dest5, 5, "%s", s.dest);
            found = 1;
        }
        if (flight8[0] && dep5[0])
            break;
    }
    return found;
}

typedef struct { char *p; size_t n, cap; } Buf;

static void bput(Buf *b, const char *s) {
    size_t l = strlen(s);
    if (b->n + l + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        while (nc < b->n + l + 1) nc *= 2;
        char *q = (char *)realloc(b->p, nc);
        if (!q) return;
        b->p = q;
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, l + 1);
    b->n += l;
}

static void bfmt(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void bfmt(Buf *b, const char *fmt, ...) {
    char t[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    bput(b, t);
}

static void airport_text(const char *icao, char *out, size_t cap) {
    AirportInfo ap;
    if (!icao[0])
        snprintf(out, cap, "не указан");
    else if (airport_db_lookup(icao, &ap) && ap.name[0])
        snprintf(out, cap, "%s - %.60s%s%.40s", icao, ap.name, ap.municipality[0] ? ", " : "", ap.municipality);
    else
        snprintf(out, cap, "%s", icao);
}

char *aircraft_report_build(const char *reg, const char *info, const char *from, const char *to,
                            const ReportMsg *m, int n) {
    Buf b = {0};
    bfmt(&b, "ВСЕ СООБЩЕНИЯ БОРТА %s\n", reg);
    if (info && info[0])
        bfmt(&b, "%s\n", info);
    bfmt(&b, "Период: %s - %s (время местное, как в журнале)\n", from && from[0] ? from : "с начала журнала",
         to && to[0] ? to : "до конца журнала");
    bfmt(&b, "Сообщений: %d\n", n);
    if (n > 0)
        bfmt(&b, "Первое: %.19s   Последнее: %.19s\n", m[0].time, m[n - 1].time);
    bput(&b, "\n");

    ReportFlight fl[32];
    int nf = aircraft_report_flights(m, n, fl, 32);
    bput(&b, "РЕЙСЫ И МАРШРУТЫ (из служебных отчётов Аэрофлота/Россия; у других авиакомпаний маршрут в сообщениях не передаётся)\n");
    if (nf == 0)
        bput(&b, "  маршрут в сообщениях не найден\n");
    for (int i = 0; i < nf; i++) {
        char a[200], d[200];
        airport_text(fl[i].dep, a, sizeof a);
        airport_text(fl[i].dest, d, sizeof d);
        bfmt(&b, "  Рейс %s: откуда %s  ->  куда %s\n", fl[i].flight[0] ? fl[i].flight : "(номер не указан)", a, d);
        bfmt(&b, "      сообщений: %d, последнее: %s\n", fl[i].count, fl[i].time);
    }
    bput(&b, "\n");

    /* последняя позиция и путь */
    int npos = 0;
    double lat = 0, lon = 0;
    char ptime[20] = "";
    int alt_ft = 0, has_alt = 0;
    for (int i = 0; i < n; i++) {
        AcarsSummary s;
        if (acars_report_summary(m[i].label, m[i].text, &s) && s.has_pos) {
            lat = s.lat; lon = s.lon;
            snprintf(ptime, sizeof ptime, "%.19s", m[i].time);
            if (s.has_alt) { alt_ft = s.alt_ft; has_alt = 1; }
            npos++;
        }
    }
    if (npos) {
        bfmt(&b, "ПОСЛЕДНЯЯ ПОЗИЦИЯ: %.5f %s, %.5f %s  (%s, всего позиций: %d)\n", lat < 0 ? -lat : lat,
             lat >= 0 ? "N" : "S", lon < 0 ? -lon : lon, lon >= 0 ? "E" : "W", ptime, npos);
        if (has_alt) {
            char at[64], num[16];
            snprintf(num, sizeof num, "%d", alt_ft);
            alt_ft_text(num, at, sizeof at, NULL);
            bfmt(&b, "  высота в этот момент: %s\n", at);
        }
        char f8[8], d5[5], e5[5];
        aircraft_report_last_route(m, n, f8, d5, e5);
        AirportInfo ap;
        if (e5[0] && airport_db_lookup(e5, &ap) && ap.has_coord)
            bfmt(&b, "  до аэропорта назначения %s по прямой: %.0f км\n", e5,
                 geo_distance_km(lat, lon, ap.lat, ap.lon));
        if (d5[0] && airport_db_lookup(d5, &ap) && ap.has_coord)
            bfmt(&b, "  от аэропорта вылета %s по прямой: %.0f км\n", d5,
                 geo_distance_km(lat, lon, ap.lat, ap.lon));
        bput(&b, "\n");
    }

    int hidden = 0;
    for (int i = 0; i < n; i++) {
        const char *t = m[i].text;
        while (*t == ' ' || *t == '\r' || *t == '\n' || *t == '\t') t++;
        if (!*t) hidden++;
    }
    bput(&b, "ХРОНОЛОГИЯ СООБЩЕНИЙ\n");
    if (hidden)
        bfmt(&b, "(без текста - служебные подтверждения «_», пустые блоки: %d, в список не включены)\n", hidden);
    char prev_day[11] = "";
    for (int i = 0; i < n; i++) {
        {
            const char *t = m[i].text;
            while (*t == ' ' || *t == '\r' || *t == '\n' || *t == '\t') t++;
            if (!*t) continue;
        }
        if (strncmp(prev_day, m[i].time, 10) != 0) {
            snprintf(prev_day, sizeof prev_day, "%.10s", m[i].time);
            bfmt(&b, "\n=== %s ===\n", prev_day);
        }
        bfmt(&b, "\n[%s] метка %s%s%s\n", m[i].time, m[i].label, m[i].label_desc[0] ? " - " : "", m[i].label_desc);
        char dec[4096];
        dec[0] = '\0';
        if (acars_report_decode(m[i].label, m[i].text, dec, sizeof dec) && dec[0]) {
            /* расшифровка с отступом */
            for (char *p = dec; *p;) {
                char *e = strchr(p, '\n');
                size_t l = e ? (size_t)(e - p) : strlen(p);
                bput(&b, "    ");
                char tmp[512];
                snprintf(tmp, sizeof tmp, "%.*s\n", (int)(l < 500 ? l : 500), p);
                bput(&b, tmp);
                if (!e) break;
                p = e + 1;
            }
        } else {
            char tmp[420];
            snprintf(tmp, sizeof tmp, "    текст: %.400s\n", m[i].text);
            bput(&b, tmp);
        }
    }
    if (n == 0)
        bput(&b, "\nСообщений этого борта за выбранный период нет.\n");
    return b.p ? b.p : strdup("");
}
