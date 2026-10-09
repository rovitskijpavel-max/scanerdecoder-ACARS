#ifndef AIRCRAFT_REPORT_H
#define AIRCRAFT_REPORT_H
#include <stddef.h>

/* Сообщение борта для отчёта (все строки - UTF-8, не NULL). time - "ГГГГ-ММ-ДД ЧЧ:ММ:СС". */
typedef struct {
    const char *time;
    const char *label;      /* метка ACARS */
    const char *label_desc; /* что означает метка (может быть "") */
    const char *text;
} ReportMsg;

/* Маршрут, найденный по сообщениям борта (последнее известное значение). */
typedef struct {
    char flight[8];
    char dep[5], dest[5];
    char time[20]; /* когда это было в последний раз */
    int count;     /* в скольких сообщениях */
} ReportFlight;

/* Собирает по сообщениям (в хронологическом порядке) рейсы с маршрутами: уникальные
 * (рейс, откуда, куда). Возвращает число записей (до max). Сообщения без рейса/маршрута пропускаются. */
int aircraft_report_flights(const ReportMsg *m, int n, ReportFlight *out, int max);

/* Последний (по времени) маршрут: dep/dest/flight из самого свежего сообщения, где они есть.
 * Возвращает 1, если нашлось хоть что-то. */
int aircraft_report_last_route(const ReportMsg *m, int n, char *flight8, char *dep5, char *dest5);

/* Полный текстовый отчёт: сводка, рейсы/маршруты, последняя позиция, хронология сообщений
 * с расшифровкой. Возвращает malloc-буфер (UTF-8, перевод строки "\n") - освободить free(). */
char *aircraft_report_build(const char *reg, const char *aircraft_info, const char *from, const char *to,
                            const ReportMsg *m, int n);

#endif
