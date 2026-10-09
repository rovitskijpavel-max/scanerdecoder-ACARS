#ifndef MAP_VIEW_H
#define MAP_VIEW_H

#include <windows.h>
#include "geomap.h"

/* Поставщик данных для карты: заполняет out (до max_out бортов) и
 * возвращает их число. Окно карты само вызывает его при открытии и
 * раз в пару секунд, пока открыто - так самолёты обновляются по мере
 * приёма новых сообщений, без нажатия каких-либо кнопок. Вызывается
 * из потока интерфейса. */
typedef int (*MapProviderFn)(MapAircraft *out, int max_out, long cutoff);
/* cutoff == 0 - текущее состояние; иначе - состояние на этот момент (секунды по geo_time_parse): сообщения позже
 * него не учитываются (проигрывание истории). */

/* Границы времени журнала (секунды, как в geo_time_parse) для ползунка проигрывания; 0/0 - нет данных. */
typedef void (*MapRangeFn)(long *t_min, long *t_max);
void mapview_set_range_cb(MapRangeFn fn);

/* Открывает (при первом вызове создаёт) окно встроенной карты и
 * подгоняет вид под все борта. parent - главное окно (для значка). */
void mapview_show(HWND parent, MapProviderFn provider);

/* Обработчик "Все сообщения борта": окно карты вызывает его с регистрацией выбранного борта
 * (без ведущей точки). Реализует главное окно - у него есть полный журнал сообщений. */
typedef void (*MapReportFn)(HWND owner, const char *reg);
void mapview_set_report_cb(MapReportFn fn);

#endif
