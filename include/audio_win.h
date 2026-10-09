#ifndef AUDIO_WIN_H
#define AUDIO_WIN_H

#ifdef _WIN32

/* Печатает список устройств записи звука в stdout (используется
 * консольным режимом --list-devices). */
int list_audio_devices(void);

/* То же самое, но программно - для GUI (заполнение выпадающего
 * списка), без обращения к stdout. */
int get_audio_device_count(void);
int get_audio_device_name(int index, char *buf, int buflen);

/* Живой приём с устройства device_index (-1 = устройство по умолчанию).
 * Печатает расшифрованные сообщения в stdout по мере приёма (в GUI-
 * режиме stdout перенаправлен в окно вывода - см. gui_win.c).
 *
 * stop_flag: если NULL - работает "вечно" (для консольного режима -l,
 * где выход - Ctrl+C). Если указатель передан - каждые ~200 мс
 * проверяет *stop_flag и корректно завершает приём (закрывает
 * устройство), как только там появится ненулевое значение - так GUI
 * останавливает приём по кнопке "Остановить", не убивая поток жёстко. */
int run_live_capture(int device_index, volatile int *stop_flag);

#endif
#endif
