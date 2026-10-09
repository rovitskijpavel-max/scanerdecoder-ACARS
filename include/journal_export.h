#ifndef JOURNAL_EXPORT_H
#define JOURNAL_EXPORT_H

/*
 * Выгрузка журнала (JSONL, который программа пишет сама) с заданного момента времени - в новый
 * JSONL или в файл Excel (.xlsx), где всё разложено по колонкам и переведено в читаемый вид.
 *
 * Время в журнале - местное время компьютера (так программа его записывает), поэтому и границы
 * периода задаются в местном времени. Повторы (одна и та же строка в нескольких файлах) отбрасываются.
 */

typedef struct {
    int read;     /* строк прочитано (с повторами) */
    int dups;     /* повторов пропущено */
    int skipped;  /* вне периода или служебных (если включён отбор) */
    int written;  /* строк записано */
    int aircraft; /* бортов в сводке (только Excel) */
} ExportStats;

typedef struct {
    const char *from;      /* "ГГГГ-ММ-ДД ЧЧ:ММ:СС" или NULL - с самого начала */
    const char *to;        /* то же - по какое время включительно, NULL - до конца */
    int skip_service;      /* 1 - не выгружать подтверждения (_), проверку канала (Q0) и SQ */
    const char *reg;       /* только этот борт ("RA73104", ".B-1073" - точка и дефис не важны), NULL/"" - все */
} ExportFilter;

/* Разбор времени, которое ввёл человек: "2026-10-06 14:30", "06.10.2026 14:30", "06.10.2026",
 * "14:30" (сегодня), "14:30:15". Результат - "ГГГГ-ММ-ДД ЧЧ:ММ:СС". round_up=1 - если секунд/минут нет,
 * подставить конец (для границы "по"): "06.10.2026" -> 23:59:59. Возвращает 1 при успехе. */
int journal_parse_time(const char *in, int round_up, char out[20]);

/* in_paths - журналы JSONL (UTF-8-пути); отсутствующие файлы пропускаются. Возвращает 0 при успехе,
 * -1 - ни один журнал не открылся, -2 - не удалось создать выходной файл. */
int journal_export_jsonl(const char *const *in_paths, int n_in, const char *out_path,
                         const ExportFilter *flt, ExportStats *st);
int journal_export_xlsx(const char *const *in_paths, int n_in, const char *out_path,
                        const ExportFilter *flt, ExportStats *st);

/* Текстовый отчёт по ОДНОМУ борту за период (flt->from/to) из всех журналов: сводка, рейсы с маршрутами,
 * последняя позиция и хронология с расшифровкой. Возвращает malloc-строку UTF-8 (free) или NULL, если ни
 * один журнал не открылся. st->written - сколько сообщений борта в отчёте. */
char *journal_aircraft_report(const char *const *in_paths, int n_in, const char *reg,
                              const ExportFilter *flt, ExportStats *st);

#endif
