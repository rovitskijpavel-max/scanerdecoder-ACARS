#ifndef MSG_SINK_H
#define MSG_SINK_H

#include "framesync.h"

/*
 * Необязательный "приёмник" СТРУКТУРИРОВАННЫХ сообщений - в дополнение
 * к печати текста (print_message) и записи в history_log.c. Нужен GUI
 * для табличного вида (gui_win.c): парсить обратно уже отформатированный
 * текст из лога было бы хрупко, а так структура AcarsMessage передаётся
 * как есть, без потерь.
 *
 * Если приёмник не зарегистрирован (msg_sink_register не вызывался -
 * консольный режим без GUI), msg_sink_notify() ничего не делает, кроме
 * мгновенной проверки указателя - расходов почти нет. source - та же
 * короткая метка источника, что передаётся в history_log_message
 * (например, "live", имя файла).
 */
typedef void (*msg_sink_cb)(const AcarsMessage *m, const char *source);

void msg_sink_register(msg_sink_cb cb);
void msg_sink_notify(const AcarsMessage *m, const char *source);

#endif
