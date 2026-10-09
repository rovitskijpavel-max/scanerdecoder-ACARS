/* Тест выгрузки: test_export <журнал.jsonl> <out.xlsx> <out.jsonl> */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "journal_export.h"
int main(int argc, char **argv) {
    if (argc < 4) return 2;
    const char *in[1] = {argv[1]};
    char a[20], b[20];
    int ok = journal_parse_time("29.09.2026 13:30", 0, a) && journal_parse_time("29.09.2026", 1, b);
    printf("parse: %d %s | %s\n", ok, a, b);
    ExportFilter f = {a, NULL, 1};
    ExportStats st;
    int r = journal_export_xlsx(in, 1, argv[2], &f, &st);
    printf("xlsx r=%d read=%d dups=%d skipped=%d written=%d aircraft=%d\n", r, st.read, st.dups, st.skipped, st.written, st.aircraft);
    ExportFilter g = {a, b, 0};
    r = journal_export_jsonl(in, 1, argv[3], &g, &st);
    printf("jsonl r=%d read=%d dups=%d skipped=%d written=%d\n", r, st.read, st.dups, st.skipped, st.written);
    /* отчёт по одному борту за все дни и за узкий период */
    ExportFilter all = {NULL, NULL, 0, NULL};
    char *rep = journal_aircraft_report(in, 1, "RA89064", &all, &st);
    int okr = rep && strstr(rep, "ULLI") && strstr(rep, "URWW") && st.written > 0;
    printf("report all: written=%d ok=%d\n", st.written, okr);
    free(rep);
    ExportFilter none = {"2030-01-01 00:00:00", NULL, 0, NULL};
    rep = journal_aircraft_report(in, 1, ".RA89064", &none, &st);
    printf("report future: written=%d (expect 0)\n", st.written);
    free(rep);
    ExportFilter one = {NULL, NULL, 1, "ra-89064"};
    r = journal_export_jsonl(in, 1, "/tmp/one.jsonl", &one, &st);
    printf("jsonl one aircraft: written=%d\n", st.written);
    return okr ? 0 : 1;
}
