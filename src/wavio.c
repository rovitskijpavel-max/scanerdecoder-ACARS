#include "wavio.h"
#include "fileio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#pragma pack(push, 1)
typedef struct {
    char riff[4];
    uint32_t size;
    char wave[4];
} RiffHeader;
typedef struct {
    char id[4];
    uint32_t size;
} ChunkHeader;
typedef struct {
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} FmtChunk;
#pragma pack(pop)

/* Открывает WAV, разбирает fmt/data чанки, оставляет файл открытым и
 * позиционированным на начало сырых данных. Возвращает 0 при успехе. */
static int wav_open_and_parse(const char *path, FILE **out_f, FmtChunk *out_fmt,
                               uint32_t *out_data_size) {
    FILE *f = fopen_utf8(path, "rb"); /* путь из окна - в UTF-8 (см. fileio.h) */
    if (!f) {
        fprintf(stderr, "Не удалось открыть файл: %s\n", path);
        return -1;
    }

    RiffHeader rh;
    if (fread(&rh, sizeof(rh), 1, f) != 1 || memcmp(rh.riff, "RIFF", 4) != 0 ||
        memcmp(rh.wave, "WAVE", 4) != 0) {
        fprintf(stderr, "Это не WAV-файл: %s\n", path);
        fclose(f);
        return -1;
    }

    FmtChunk fmt;
    memset(&fmt, 0, sizeof(fmt));
    long data_pos = -1;
    uint32_t data_size = 0;

    while (1) {
        ChunkHeader ch;
        if (fread(&ch, sizeof(ch), 1, f) != 1)
            break;
        if (memcmp(ch.id, "fmt ", 4) == 0) {
            size_t to_read = ch.size < sizeof(fmt) ? ch.size : sizeof(fmt);
            if (fread(&fmt, to_read, 1, f) != 1)
                break;
            if (ch.size > to_read)
                fseek(f, (long)(ch.size - to_read), SEEK_CUR);
        } else if (memcmp(ch.id, "data", 4) == 0) {
            data_pos = ftell(f);
            data_size = ch.size;
            fseek(f, (long)ch.size, SEEK_CUR);
        } else {
            fseek(f, (long)ch.size, SEEK_CUR);
        }
        if (ch.size % 2 == 1) /* выравнивание по границе слова */
            fseek(f, 1, SEEK_CUR);
    }

    if (data_pos < 0 || fmt.num_channels == 0) {
        fprintf(stderr, "В WAV-файле не найдены нужные чанки (fmt/data)\n");
        fclose(f);
        return -1;
    }

    fseek(f, data_pos, SEEK_SET);

    *out_f = f;
    *out_fmt = fmt;
    *out_data_size = data_size;
    return 0;
}

static double decode_sample(const uint8_t *sp, int bits) {
    if (bits == 16) {
        int16_t s = (int16_t)(sp[0] | (sp[1] << 8));
        return s / 32768.0;
    } else if (bits == 8) {
        return (sp[0] - 128) / 128.0;
    } else if (bits == 32) {
        int32_t s = (int32_t)(sp[0] | (sp[1] << 8) | (sp[2] << 16) | (sp[3] << 24));
        return s / 2147483648.0;
    }
    return 0.0;
}

int wav_get_num_channels(const char *path, int *out_channels) {
    FILE *f;
    FmtChunk fmt;
    uint32_t data_size;
    if (wav_open_and_parse(path, &f, &fmt, &data_size) != 0)
        return -1;
    fclose(f);
    *out_channels = fmt.num_channels;
    return 0;
}

int wav_load_channel(const char *path, int channel_index,
                      float **out_samples, size_t *out_count, int *out_samplerate) {
    FILE *f;
    FmtChunk fmt;
    uint32_t data_size;
    if (wav_open_and_parse(path, &f, &fmt, &data_size) != 0)
        return -1;

    int channels = fmt.num_channels;
    int bits = fmt.bits_per_sample;
    if (channel_index < 0 || channel_index >= channels) {
        fprintf(stderr, "Запрошен канал %d, а в файле их %d\n", channel_index, channels);
        fclose(f);
        return -1;
    }

    size_t bytes_per_sample = bits / 8;
    size_t frame_size = bytes_per_sample * (size_t)channels;
    size_t num_frames = frame_size ? data_size / frame_size : 0;

    float *out = (float *)malloc(num_frames * sizeof(float));
    if (!out) {
        fclose(f);
        return -1;
    }

    uint8_t *raw = (uint8_t *)malloc(frame_size);
    size_t i;
    for (i = 0; i < num_frames; i++) {
        if (fread(raw, frame_size, 1, f) != 1)
            break;
        const uint8_t *sp = raw + (size_t)channel_index * bytes_per_sample;
        out[i] = (float)decode_sample(sp, bits);
    }
    free(raw);
    fclose(f);

    *out_samples = out;
    *out_count = i;
    *out_samplerate = (int)fmt.sample_rate;
    return 0;
}

int wav_load_mono(const char *path, float **out_samples, size_t *out_count, int *out_samplerate) {
    FILE *f;
    FmtChunk fmt;
    uint32_t data_size;
    if (wav_open_and_parse(path, &f, &fmt, &data_size) != 0)
        return -1;

    int channels = fmt.num_channels;
    int bits = fmt.bits_per_sample;
    size_t bytes_per_sample = bits / 8;
    size_t frame_size = bytes_per_sample * (size_t)channels;
    size_t num_frames = frame_size ? data_size / frame_size : 0;

    float *out = (float *)malloc(num_frames * sizeof(float));
    if (!out) {
        fclose(f);
        return -1;
    }

    uint8_t *raw = (uint8_t *)malloc(frame_size);
    size_t i;
    for (i = 0; i < num_frames; i++) {
        if (fread(raw, frame_size, 1, f) != 1)
            break;
        double acc = 0.0;
        for (int c = 0; c < channels; c++)
            acc += decode_sample(raw + (size_t)c * bytes_per_sample, bits);
        out[i] = (float)(acc / channels);
    }
    free(raw);
    fclose(f);

    *out_samples = out;
    *out_count = i;
    *out_samplerate = (int)fmt.sample_rate;
    return 0;
}

int resample_linear(const float *in, size_t in_count, int in_rate,
                     int target_rate, float **out, size_t *out_count) {
    if (in_rate == target_rate) {
        float *copy = (float *)malloc(in_count * sizeof(float));
        memcpy(copy, in, in_count * sizeof(float));
        *out = copy;
        *out_count = in_count;
        return 0;
    }

    double ratio = (double)target_rate / (double)in_rate;
    size_t n_out = (size_t)((double)in_count * ratio);
    float *out_buf = (float *)malloc(n_out * sizeof(float));
    if (!out_buf)
        return -1;

    for (size_t i = 0; i < n_out; i++) {
        double src_pos = (double)i / ratio;
        size_t idx = (size_t)src_pos;
        double frac = src_pos - (double)idx;
        float a = (idx < in_count) ? in[idx] : 0.0f;
        float b = (idx + 1 < in_count) ? in[idx + 1] : a;
        out_buf[i] = (float)(a + (b - a) * frac);
    }

    *out = out_buf;
    *out_count = n_out;
    return 0;
}
