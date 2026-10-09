#ifndef APP_H
#define APP_H

/* Декодирует один WAV-файл (все каналы, если их несколько) и печатает
 * найденные сообщения в stdout (в GUI-режиме stdout перенаправлен в
 * окно вывода - см. gui_win.c). if_mode: 0 = автоопределение "сырой
 * IF/готовый звук", 1 = всегда IF, -1 = всегда готовый звук. verbose:
 * подробный вывод резервного конвейера (всплески, фазы и т.п.). */
int decode_wav_file(const char *path, int if_mode, int verbose);

#endif
