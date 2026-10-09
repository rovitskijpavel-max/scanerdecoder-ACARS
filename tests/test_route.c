/* test_route <журнал.jsonl> <борт> : отчёт по борту + проверка координат AFL-сообщений относительно аэропортов */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aircraft_report.h"
#include "acars_report.h"
#include "geomap.h"

static int jget(const char *line, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(line, pat);
    if (!p) { out[0] = 0; return 0; }
    p += strlen(pat);
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            char c = *p;
            if (c == 'n') c = '\n'; else if (c == 'r') c = '\r'; else if (c == 't') c = '\t';
            else if (c == 'u') { p += 4; c = '?'; }
            out[o++] = c; p++;
        } else out[o++] = *p++;
    }
    out[o] = 0;
    return 1;
}

typedef struct { const char *icao; double lat, lon; } Apt;
static const Apt APT[] = {
    {"UUEE", 55.9726, 37.4146}, {"ULLI", 59.8003, 30.2625}, {"URWW", 48.7825, 44.3455},
    {"UUWW", 55.5915, 37.2615}, {"UWGG", 56.2301, 43.7840}, {"UUDD", 55.4088, 37.9063},
    {"URSS", 43.4499, 39.9566}, {"LTFM", 41.2753, 28.7519},
};

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "r");
    if (!f) return 2;
    static char line[20000];
    ReportMsg *ms = calloc(5000, sizeof *ms);
    int n = 0, checked = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        char addr[40], t[40], lab[8], ld[200], tx[8000];
        jget(line, "addr", addr, sizeof addr); jget(line, "timestamp", t, sizeof t);
        jget(line, "label", lab, sizeof lab); jget(line, "label_description", ld, sizeof ld);
        jget(line, "text", tx, sizeof tx);
        /* проверка координат: сообщение с позицией и событием на земле/рядом с аэропортом */
        AcarsSummary s;
        if (acars_report_summary(lab, tx, &s) && s.has_pos) {
            double lat = s.lat, lon = s.lon;
            if (lat < -90 || lat > 90 || lon < -180 || lon > 180) { bad++; printf("BAD range %s\n", t); }
            for (size_t k = 0; k < sizeof APT / sizeof *APT; k++) {
                if (!strcmp(s.dep, APT[k].icao) || !strcmp(s.dest, APT[k].icao)) {
                    double d = geo_distance_km(lat, lon, APT[k].lat, APT[k].lon);
                    if (d < 60 && !strcmp(s.type, "OFF")) { checked++; printf("OFF/%s near %s: %.1f km\n", s.type, APT[k].icao, d); }
                }
            }
        }
        if (argc > 2 && strstr(addr, argv[2]) && n < 5000) {
            ms[n].time = strdup(t); ms[n].label = strdup(lab); ms[n].label_desc = strdup(ld); ms[n].text = strdup(tx);
            n++;
        }
    }
    printf("coordinate checks: near-airport=%d bad=%d\n", checked, bad);
    if (argc > 2) {
        char *r = aircraft_report_build(argv[2], "тест", NULL, NULL, ms, n);
        printf("%s\n", r);
        free(r);
    }
    return bad != 0;
}
