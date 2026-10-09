#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "demod.h"
#include "framesync.h"
#include "crc16.h"
#include "wavio.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static uint8_t add_parity(uint8_t ch7) {
    int c = 0;
    uint8_t b = ch7 & 0x7F;
    for (int i = 0; i < 7; i++)
        if (b & (1 << i))
            c++;
    if (c % 2 == 0)
        b |= 0x80; /* добиваем до нечётного количества единиц */
    return b;
}

static int g_ok = 0;
static char g_got_text[512];
static int g_got_crc_ok = 0;

static void on_bit(int bit, void *ctx) {
    framesync_feed_bit((FrameSync *)ctx, bit);
}

static void on_msg(const AcarsMessage *m, void *ctx) {
    (void)ctx;
    g_ok = 1;
    g_got_crc_ok = m->crc_ok;
    strncpy(g_got_text, m->text, sizeof(g_got_text) - 1);
    printf("Декодировано сообщение: mode=%c addr=%s label=%s crc_ok=%d\n",
           m->mode, m->addr, m->label, m->crc_ok);
    printf("Текст: \"%s\"\n", m->text);
}

int main(void) {
    /* 1. Преамбула кадра (SYN SYN SOH) - служебные байты синхронизации,
     *    передаются в эфир, но НЕ входят в контрольную сумму. */
    uint8_t preamble[3];
    preamble[0] = add_parity(0x16); /* SYN */
    preamble[1] = add_parity(0x16); /* SYN */
    preamble[2] = add_parity(0x01); /* SOH */

    /* 2. Содержимое кадра (Mode..ETX включительно) - вот это уже входит
     *    в BCS/CRC, ровно как в настоящем ACARS. */
    uint8_t content[128];
    int cl = 0;
    content[cl++] = add_parity('2');  /* mode */
    const char *addr = ".TEST12";
    for (int i = 0; i < 7; i++) content[cl++] = add_parity(addr[i]);
    content[cl++] = add_parity(' '); /* ack/nak */
    content[cl++] = add_parity('H');
    content[cl++] = add_parity('1'); /* label H1 */
    content[cl++] = add_parity(0x02); /* STX */
    const char *text = "TEST ACARS MESSAGE 12345";
    for (const char *p = text; *p; p++) content[cl++] = add_parity(*p);
    content[cl++] = add_parity(0x03); /* ETX */

    /* 2. Считаем CRC и пробуем оба порядка байт (lo,hi) и (hi,lo) -
     *    определяем, какой даёт самосогласованный результат (0) */
    uint16_t crc = crc16_buffer(content, cl);
    uint8_t lo = crc & 0xFF, hi = (crc >> 8) & 0xFF;

    uint8_t frame_lohi[130];
    memcpy(frame_lohi, content, cl);
    frame_lohi[cl] = lo;
    frame_lohi[cl + 1] = hi;
    uint16_t check_lohi = crc16_buffer(frame_lohi, cl + 2);

    uint8_t frame_hilo[130];
    memcpy(frame_hilo, content, cl);
    frame_hilo[cl] = hi;
    frame_hilo[cl + 1] = lo;
    uint16_t check_hilo = crc16_buffer(frame_hilo, cl + 2);

    printf("CRC = 0x%04X, проверка lo-hi = 0x%04X, проверка hi-lo = 0x%04X\n",
           crc, check_lohi, check_hilo);

    uint8_t *frame;
    int frame_len;
    if (check_lohi == 0) {
        frame = frame_lohi;
        frame_len = cl + 2;
        printf("-> порядок байт BCS: младший байт первым (lo, hi)\n");
    } else if (check_hilo == 0) {
        frame = frame_hilo;
        frame_len = cl + 2;
        printf("-> порядок байт BCS: старший байт первым (hi, lo)\n");
    } else {
        printf("ОШИБКА: ни один порядок байт CRC не даёт самосогласованности!\n");
        return 1;
    }

    /* 3. Модулируем в аудио: непрерывная фаза FSK, 1 = 2400 Гц, 0 = 1200 Гц,
     *    LSB каждого байта передаётся первым. Преамбула-несущая (тон
     *    мark для настройки АРУ/битовой синхронизации), затем сами
     *    байты SYN SYN SOH (для трёхступенчатой синхронизации в
     *    framesync.c), затем содержимое кадра + BCS. */
    int fs = MSK_TARGET_FS;
    int spb = MSK_SAMPLES_PER_BIT;
    int preamble_bits = 100;
    int total_bits = preamble_bits + 3 * 8 + frame_len * 8 + 16; /* +хвост тишины/несущей */
    int total_samples = total_bits * spb;
    float *audio = (float *)malloc(total_samples * sizeof(float));

    double phase = 0.0;
    int si = 0;
    /* NRZI-кодирование при передаче (зеркально NRZI-декодированию в
     * demod.c): текущий тон меняется, только когда очередной бит
     * данных равен 0; остаётся прежним, когда бит равен 1. Преамбула
     * - это тон 2400 Гц ("1"), поэтому стартовое состояние - тоже "1". */
    int tone_state = 1;
    for (int i = 0; i < preamble_bits; i++) {
        double freq = 2400.0;
        double step = 2.0 * M_PI * freq / fs;
        for (int s = 0; s < spb; s++) {
            audio[si++] = (float)sin(phase);
            phase += step;
            if (phase > 2 * M_PI) phase -= 2 * M_PI;
        }
    }
    for (int bi = 0; bi < 3; bi++) {
        uint8_t byte = preamble[bi];
        for (int b = 0; b < 8; b++) {
            int data_bit = (byte >> b) & 1;
            if (!data_bit)
                tone_state = !tone_state;
            double freq = tone_state ? 2400.0 : 1200.0;
            double step = 2.0 * M_PI * freq / fs;
            for (int s = 0; s < spb; s++) {
                audio[si++] = (float)sin(phase);
                phase += step;
                if (phase > 2 * M_PI) phase -= 2 * M_PI;
            }
        }
    }
    for (int bi = 0; bi < frame_len; bi++) {
        uint8_t byte = frame[bi];
        for (int b = 0; b < 8; b++) {
            int data_bit = (byte >> b) & 1;
            if (!data_bit)
                tone_state = !tone_state;
            double freq = tone_state ? 2400.0 : 1200.0;
            double step = 2.0 * M_PI * freq / fs;
            for (int s = 0; s < spb; s++) {
                audio[si++] = (float)sin(phase);
                phase += step;
                if (phase > 2 * M_PI) phase -= 2 * M_PI;
            }
        }
    }
    for (int i = 0; i < 16; i++) {
        double freq = 1800.0;
        double step = 2.0 * M_PI * freq / fs;
        for (int s = 0; s < spb; s++) {
            audio[si++] = 0.0f;
            phase += step;
        }
    }

    /* 4. Пропускаем через реальный демодулятор + парсер кадров */
    FrameSync framesync;
    framesync_init(&framesync, on_msg, NULL);
    MskDemod demod;
    msk_init(&demod, on_bit, &framesync);
    msk_process(&demod, audio, si);

    printf("\n--- Результат самотеста ---\n");
    if (g_ok && g_got_crc_ok && strcmp(g_got_text, text) == 0) {
        printf("УСПЕХ: конвейер демодуляции и разбора кадра работает корректно.\n");
    } else {
        printf("ПРОВАЛ: got_msg=%d crc_ok=%d text_match=%d\n",
               g_ok, g_got_crc_ok, g_ok ? strcmp(g_got_text, text) == 0 : -1);
    }

    /* Заодно сохраним тестовый сигнал в WAV, чтобы можно было послушать
     * и сравнить с реальной записью с приёмника */
    FILE *f = fopen("test_signal.wav", "wb");
    if (f) {
        int16_t *pcm = (int16_t *)malloc(si * sizeof(int16_t));
        for (int i = 0; i < si; i++) {
            float v = audio[i] * 0.8f;
            if (v > 1) v = 1;
            if (v < -1) v = -1;
            pcm[i] = (int16_t)(v * 32767.0f);
        }
        uint32_t data_bytes = si * sizeof(int16_t);
        uint32_t riff_size = 36 + data_bytes;
        uint16_t ch = 1, bits = 16, fmt_tag = 1;
        uint32_t sr = fs;
        uint32_t byte_rate = sr * ch * bits / 8;
        uint16_t block_align = ch * bits / 8;
        fwrite("RIFF", 1, 4, f);
        fwrite(&riff_size, 4, 1, f);
        fwrite("WAVE", 1, 4, f);
        fwrite("fmt ", 1, 4, f);
        uint32_t fmt_size = 16;
        fwrite(&fmt_size, 4, 1, f);
        fwrite(&fmt_tag, 2, 1, f);
        fwrite(&ch, 2, 1, f);
        fwrite(&sr, 4, 1, f);
        fwrite(&byte_rate, 4, 1, f);
        fwrite(&block_align, 2, 1, f);
        fwrite(&bits, 2, 1, f);
        fwrite("data", 1, 4, f);
        fwrite(&data_bytes, 4, 1, f);
        fwrite(pcm, sizeof(int16_t), si, f);
        fclose(f);
        free(pcm);
        printf("Тестовый сигнал сохранён в test_signal.wav\n");
    }

    free(audio);
    return (g_ok && g_got_crc_ok) ? 0 : 1;
}
