#ifndef FRAMESYNC_H
#define FRAMESYNC_H

#include <stdint.h>

#define ACARS_MAX_PAYLOAD 260

typedef struct {
    int crc_ok;
    int crc_fixed;   /* 1, если исходный CRC не сошёлся, но нашлась и была
                      * применена правка (см. bits_fixed), после которой CRC
                      * сошёлся. */
    int bits_fixed;  /* сколько бит было исправлено: 0 (не потребовалось),
                      * 1 или 2. Коррекция направляется чётностью символов
                      * (см. try_fix_bits в framesync.c) - это резко снижает
                      * риск случайного ложного "исправления" по сравнению
                      * со слепым перебором всех битов кадра, особенно
                      * важно для 2-битных случаев (кандидатов гораздо
                      * больше, чем при одном бите). */
    uint8_t raw[ACARS_MAX_PAYLOAD]; /* весь кадр от SOH до конца BCS включительно */
    int raw_len;

    char mode;
    char addr[8];   /* 7 символов + '\0' */
    char acknak;    /* '!'=ACK, символ NAK, или пусто */
    char label[3];  /* 2 символа + '\0' */
    char block_id;
    char text[ACARS_MAX_PAYLOAD];
    int has_text;
    int is_etb; /* 1, если сообщение завершено ETB (многоблоковое / без текста) */
} AcarsMessage;

typedef void (*acars_msg_cb)(const AcarsMessage *msg, void *ctx);

typedef enum {
    FS_WAIT_SYN,  /* ищем 0x16 (SYN), проверяя КАЖДЫЙ бит (скользящее окно) */
    FS_SYN2,      /* ждём второй байт SYN, уже побайтово выровненно */
    FS_SEEK_SOH,  /* ждём SOH сразу после SYN SYN, тоже выровненно */
    FS_IN_FRAME   /* собираем байты кадра до ETX/ETB + 2 байта BCS */
} FsState;

typedef struct {
    FsState state;
    uint8_t shift;   /* скользящий 8-битный регистр (обновляется каждым битом) */
    int bit_pos;     /* сколько НОВЫХ бит накоплено с последнего выровненного байта */
    int invert;      /* инверсия полярности бит (после обнаружения ~SYN - 180-градусная
                       * фазовая неоднозначность MSK, типичный эффект для реального
                       * сигнала, отсутствующий в идеализированной синтетике) */

    uint8_t buf[ACARS_MAX_PAYLOAD];
    int buf_len;

    int got_suffix;
    int suffix_crc_bytes_left;

    acars_msg_cb cb;
    void *cb_ctx;
} FrameSync;

void framesync_init(FrameSync *fs, acars_msg_cb cb, void *ctx);
void framesync_feed_bit(FrameSync *fs, int bit);

#endif
