#ifndef WAVIO_H
#define WAVIO_H

#include <stddef.h>

/* Загружает WAV (PCM 8/16/32-бит). Если файл моно - просто отдаёт его
 * отсчёты. Если файл многоканальный - УСРЕДНЯЕТ каналы в один поток.
 * Это подходит для звука с одного микрофона/приёмника, записанного по
 * ошибке в стерео и т.п., но НЕ подходит для записей, где разные
 * каналы - это разные независимые радиоканалы (как в тестовом
 * `test.wav` проекта acarsdec: 4 канала = 4 независимых частоты) -
 * усреднение в таком случае смешает разные сообщения в кашу. Для таких
 * файлов используйте wav_get_num_channels() + wav_load_channel() ниже.
 * Возвращает 0 при успехе. *out_samples нужно освободить free(). */
int wav_load_mono(const char *path, float **out_samples, size_t *out_count, int *out_samplerate);

/* Узнать число каналов в WAV-файле, не читая все данные. */
int wav_get_num_channels(const char *path, int *out_channels);

/* Загружает ОДИН конкретный канал (0-индексация) многоканального
 * WAV-файла, без усреднения с остальными каналами. Для моно-файлов
 * channel_index должен быть 0 - ведёт себя как wav_load_mono(). */
int wav_load_channel(const char *path, int channel_index,
                      float **out_samples, size_t *out_count, int *out_samplerate);

/* Простая линейная передискретизация в target_rate (для нас всегда 48000,
 * чтобы в демодуляторе было ровно 20 отсчётов на бит). */
int resample_linear(const float *in, size_t in_count, int in_rate,
                     int target_rate, float **out, size_t *out_count);

#endif
