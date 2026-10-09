#include <stdio.h>
#include "framesync.h"
#include "crc16.h"
#include <string.h>

#define SYN 0x16
#define SYN_INV ((uint8_t)(~SYN)) /* 0xE9 - "инвертированный" SYN, признак
                                    * 180-градусной фазовой неоднозначности
                                    * MSK-демодулятора на реальном сигнале */
#define SOH 0x01
#define STX 0x02
#define ETX 0x03
#define ETB 0x17

static void reset_to_wait_syn(FrameSync *fs) {
    fs->state = FS_WAIT_SYN;
    fs->bit_pos = 0;
    fs->buf_len = 0;
    fs->got_suffix = 0;
    fs->suffix_crc_bytes_left = 0;
    /* shift и invert НЕ трогаем: значение shift может уже содержать
     * начало следующего валидного SYN (даём FS_WAIT_SYN шанс поймать
     * его с первого же следующего бита, а не ждать заново 8 бит) -
     * ровно как в оригинальном acarsdec (resetAcars не чистит регистр
     * байта). invert - это состояние демодулятора/канала, а не кадра,
     * сбрасывать его при потере синхронизации кадра не нужно. */
}

void framesync_init(FrameSync *fs, acars_msg_cb cb, void *ctx) {
    memset(fs, 0, sizeof(*fs));
    fs->cb = cb;
    fs->cb_ctx = ctx;
    fs->state = FS_WAIT_SYN;
}

static void copy_masked(char *dst, const uint8_t *src, int len) {
    for (int i = 0; i < len; i++)
        dst[i] = (char)(src[i] & 0x7F);
    dst[len] = '\0';
}

/* Чётность байта (считая ВСЕ 8 бит, включая сам бит чётности) - по
 * конвенции ACARS каждый символ передаётся с НЕЧЁТНОЙ чётностью (см.
 * add_parity() в selftest.c): у корректно принятого байта суммарное
 * число единичных бит должно быть НЕЧЁТНЫМ. Чётное число - явный
 * признак (нечётного количества, чаще всего одного) битового сбоя
 * именно в этом байте. */
static int byte_parity_ok(uint8_t b) {
    int ones = 0;
    for (int i = 0; i < 8; i++)
        if (b & (1u << i))
            ones++;
    return (ones % 2) == 1;
}

/* Перебирает все buf_len*8 однобитовых правок по всему кадру и
 * проверяет, обнуляется ли CRC. Резервный, "слепой" метод - применяется
 * только если чётность символов ничего не подсказала (см.
 * try_fix_bits ниже): дороже (O(n) попыток вместо O(1)-O(8) при
 * chётности), но для одного бита риск случайного совпадения CRC
 * невелик (~n/65536 при n <= 2000 бит кадра). Возвращает 1 и применяет
 * правку, если найден РОВНО один подходящий бит. */
static int try_fix_single_bit_blind(uint8_t *buf, int buf_len) {
    int fix_byte = -1, fix_bit = -1, matches = 0;

    for (int i = 0; i < buf_len; i++) {
        for (int b = 0; b < 8; b++) {
            buf[i] ^= (uint8_t)(1u << b);
            if (crc16_buffer(buf, (size_t)buf_len) == 0) {
                matches++;
                if (matches == 1) {
                    fix_byte = i;
                    fix_bit = b;
                }
            }
            buf[i] ^= (uint8_t)(1u << b); /* возвращаем как было */
        }
    }

    if (matches == 1) {
        buf[fix_byte] ^= (uint8_t)(1u << fix_bit);
        return 1;
    }
    return 0;
}

/*
 * Основная коррекция ошибок: направляется чётностью символов, а не
 * слепым перебором всего кадра - резко уменьшает пространство поиска
 * и, что важнее, риск случайного ложного "исправления" (у CRC-16
 * всего 65536 возможных значений; при переборе ~2000*2000/2 = 2 млн.
 * пар бит по всему кадру шанс на случайное совпадение почти
 * гарантирован - а вот при переборе 8 или 8*8=64 кандидатов,
 * подсказанных чётностью КОНКРЕТНЫХ байт, риск пренебрежимо мал).
 *
 *   0 "подозрительных" байт (чётность верна везде) - ошибка (если она
 *     вообще есть) не сводится к нечётному числу сбоев в одном байте;
 *     подстраховываемся старым слепым перебором ОДНОГО бита по всему
 *     кадру (см. try_fix_single_bit_blind).
 *   1 "подозрительный" байт - перебираем 8 бит именно в нём.
 *   2 "подозрительных" байта - перебираем 8x8=64 комбинации (по одному
 *     биту в каждом) - реальная 2-битная коррекция, ключевое отличие
 *     от предыдущей версии, которая умела исправлять только 1 бit.
 *   3+ "подозрительных" байт - кадр слишком испорчен, чтобы доверять
 *     исправлению; не трогаем.
 *
 * Возвращает число исправленных бит (0, 1 или 2); buf изменяется на
 * месте только при успехе (return > 0).
 */
static int try_fix_bits(uint8_t *buf, int buf_len) {
    /* Чётность НЕЧЁТНАЯ действует только для символов содержимого
     * (Mode/Addr/Ack/Label/текст и т.п.) - это правило кодирования
     * конкретно печатаемых/управляющих 7-битных ASCII-символов. Два
     * последних байта кадра - это сырое 16-битное значение CRC/BCS,
     * а не символ с битом чётности; проверять их "чётность" бессмысленно
     * и даже вредно - случайное несовпадение там ложно раздувало бы
     * число "подозрительных" байт и сбивало бы алгоритм ниже. */
    int content_len = buf_len - 2;
    int suspects[8];
    int n_suspects = 0;
    for (int i = 0; i < content_len && n_suspects <= 3; i++) {
        if (!byte_parity_ok(buf[i])) {
            if (n_suspects < 8)
                suspects[n_suspects] = i;
            n_suspects++;
        }
    }

    if (n_suspects == 1) {
        int byte_i = suspects[0];
        for (int b = 0; b < 8; b++) {
            buf[byte_i] ^= (uint8_t)(1u << b);
            if (crc16_buffer(buf, (size_t)buf_len) == 0)
                return 1; /* чётность сама по себе уже сузила выбор до 8
                           * вариантов - дополнительная проверка на
                           * единственность здесь избыточна */
            buf[byte_i] ^= (uint8_t)(1u << b);
        }
        /* чётность указала на байт, но ни один бит внутри него не
         * подошёл по CRC - похоже, ошибок больше одной; не гадаем. */
    } else if (n_suspects == 2) {
        int byte_a = suspects[0], byte_b = suspects[1];
        for (int ba = 0; ba < 8; ba++) {
            buf[byte_a] ^= (uint8_t)(1u << ba);
            for (int bb = 0; bb < 8; bb++) {
                buf[byte_b] ^= (uint8_t)(1u << bb);
                if (crc16_buffer(buf, (size_t)buf_len) == 0)
                    return 2;
                buf[byte_b] ^= (uint8_t)(1u << bb);
            }
            buf[byte_a] ^= (uint8_t)(1u << ba);
        }
    }

    /* Чётность не дала однозначной подсказки (0 подозрительных байт,
     * либо подсказка не подтвердилась по CRC) - подстраховываемся
     * прежним слепым перебором одного бита по всему кадру. */
    if (try_fix_single_bit_blind(buf, buf_len))
        return 1;

    return 0;
}

static int is_ascii_printable(char c) {
    unsigned char u = (unsigned char)c;
    return u >= 0x20 && u <= 0x7E;
}

/* Даже когда CRC "сошёлся" (в т.ч. после подбора одного бита), кадр
 * может оказаться совпадением по чистой случайности - на шумном
 * сигнале при переборе многих фаз и битовых правок это происходит не
 * так уж редко (CRC16 - это всего 65536 возможных значений). Реальные
 * поля ACARS (mode/addr/acknak/label/blockid/текст) всегда состоят из
 * печатаемых ASCII-символов; если после "успешного" CRC там мусор
 * (управляющие символы) - почти наверняка это ложное совпадение, а не
 * настоящее сообщение. */
static int fields_look_sane(const AcarsMessage *m) {
    if (m->mode && !is_ascii_printable(m->mode))
        return 0;
    for (int i = 0; m->addr[i]; i++)
        if (!is_ascii_printable(m->addr[i]))
            return 0;
    if (m->acknak && !is_ascii_printable(m->acknak) && (unsigned char)m->acknak != 0x15)
        return 0; /* 0x15 = NAK, стандартное значение поля Ack/Nak при отрицательном ответе */
    for (int i = 0; m->label[i]; i++)
        if (!is_ascii_printable(m->label[i]))
            return 0;
    if (m->block_id && !is_ascii_printable(m->block_id))
        return 0;
    if (m->has_text) {
        for (int i = 0; m->text[i]; i++) {
            char c = m->text[i];
            if (!is_ascii_printable(c) && c != '\n' && c != '\r')
                return 0;
        }
    }
    return 1;
}

static void finalize_frame(FrameSync *fs) {
    AcarsMessage msg;
    memset(&msg, 0, sizeof(msg));

    int buf_len = fs->buf_len;
    if (buf_len > ACARS_MAX_PAYLOAD)
        buf_len = ACARS_MAX_PAYLOAD;

    memcpy(msg.raw, fs->buf, buf_len);
    msg.raw_len = buf_len;

    uint16_t crc = crc16_buffer(msg.raw, buf_len);
    msg.crc_ok = (crc == 0);
    msg.crc_fixed = 0;
    msg.bits_fixed = 0;

    if (!msg.crc_ok && buf_len >= 4 && buf_len <= ACARS_MAX_PAYLOAD) {
        int fixed = try_fix_bits(msg.raw, buf_len);
        if (fixed > 0) {
            msg.crc_ok = 1;
            msg.crc_fixed = 1;
            msg.bits_fixed = fixed;
        }
    }

    if (buf_len >= 3) {
        int content_len = buf_len - 2; /* всё кроме 2 байт BCS (SOH уже не хранится в buf) */
        const uint8_t *content = msg.raw;

        if (content_len >= 1)
            msg.mode = (char)(content[0] & 0x7F);

        if (content_len >= 8)
            copy_masked(msg.addr, content + 1, 7);

        if (content_len >= 9)
            msg.acknak = (char)(content[8] & 0x7F);

        if (content_len >= 11)
            copy_masked(msg.label, content + 9, 2);

        if (content_len >= 1) {
            uint8_t suffix = content[content_len - 1];
            msg.is_etb = ((suffix & 0x7F) == ETB);
        }

        if (content_len > 12) {
            const uint8_t *rest = content + 11;
            int rest_len = content_len - 11 - 1; /* минус сам суффикс-байт */
            int stx_pos = -1;
            for (int i = 0; i < rest_len; i++) {
                if ((rest[i] & 0x7F) == STX) {
                    stx_pos = i;
                    break;
                }
            }
            if (stx_pos >= 0) {
                if (stx_pos > 0)
                    msg.block_id = (char)(rest[0] & 0x7F);
                int text_len = rest_len - stx_pos - 1;
                if (text_len > 0 && text_len < ACARS_MAX_PAYLOAD) {
                    copy_masked(msg.text, rest + stx_pos + 1, text_len);
                    msg.has_text = 1;
                }
            } else if (rest_len >= 1) {
                msg.block_id = (char)(rest[0] & 0x7F);
            }
        }
    }

    if (msg.crc_fixed && !fields_look_sane(&msg)) {
#ifdef FRAMESYNC_DEBUG_SANITY
        fprintf(stderr, "[framesync] CRC (исправленный) но поля не прошли sanity-check: mode=%c addr=%s label=%s text=%s\n",
                msg.mode, msg.addr, msg.label, msg.has_text ? msg.text : "(none)");
#endif
        /* Sanity-проверку применяем ТОЛЬКО к кадрам, где CRC сошёлся
         * после подбора бит (см. try_fix_bits) - там действительно
         * есть небольшой (но не нулевой) риск случайного совпадения
         * CRC16 на шуме при переборе кандидатов. Если же CRC сошёлся
         * СРАЗУ, без единой правки - это уже не может быть
         * случайностью (вероятность ~1/65536 на попытку, и попытка
         * ровно одна), и отбрасывать такой кадр только из-за
         * непечатаемых байт в тексте неверно: у настоящих ACARS-
         * сообщений (например, бинарные ADS-C/CPDLC поверх label Q0/SA
         * и т.п.) непечатаемые байты в тексте - совершенно законное
         * явление, а не признак ошибки. */
        msg.crc_ok = 0;
        msg.crc_fixed = 0;
        msg.bits_fixed = 0;
    }

    if (fs->cb)
        fs->cb(&msg, fs->cb_ctx);
}

/*
 * Синхронизация теперь трёхступенчатая (как в оригинальном acarsdec:
 * WaitingSyn -> Syn2 -> SeekingSoh), а не наивный поиск одного SOH:
 *   1. FS_WAIT_SYN - скользящее окно проверяется КАЖДЫЙ бит, ищем
 *      байт 0x16 (SYN). Так же ищем его битовый инверс 0xE9 - это
 *      признак 180-градусной фазовой неоднозначности MSK-демодулятора
 *      (обычное дело на реальном, а не синтетическом сигнале); если
 *      нашли инверс - переключаем fs->invert и продолжаем как обычно.
 *   2. FS_SYN2 - следующий байт (уже побайтово выровненный, не
 *      скользящий) должен ТОЖЕ быть SYN (или его инверсом), иначе
 *      сброс в FS_WAIT_SYN.
 *   3. FS_SEEK_SOH - следующий выровненный байт должен быть SOH,
 *      иначе сброс.
 *   4. FS_IN_FRAME - собираем байты как раньше, до ETX/ETB + 2 байта
 *      BCS.
 *
 * Раньше кадр начинался от ПЕРВОГО же попавшегося байта 0x01 в потоке
 * бит (в т.ч. по чистой случайности - например, ровно такой паттерн
 * "1000 0000" естественным образом возникает на границе, где долгий
 * тон преамбулы "1111...1" обрывается в "0000...") - на реальных
 * записях это раз за разом приводило к ложной синхронизации и
 * полному провалу декодирования всплеска. Требование ДВУХ подряд
 * идущих байт-выровненных SYN перед SOH резко (на 4 порядка)
 * снижает вероятность случайной ложной синхронизации.
 */
void framesync_feed_bit(FrameSync *fs, int bit) {
    if (fs->invert)
        bit = !bit;

    fs->shift = (uint8_t)((fs->shift >> 1) | (bit ? 0x80 : 0));
    fs->bit_pos++;

    if (fs->state == FS_WAIT_SYN) {
        if (fs->shift == SYN) {
            fs->state = FS_SYN2;
            fs->bit_pos = 0;
        } else if (fs->shift == SYN_INV) {
            fs->invert = !fs->invert;
            fs->state = FS_SYN2;
            fs->bit_pos = 0;
        }
        return;
    }

    if (fs->bit_pos < 8)
        return;

    uint8_t byte = fs->shift;
    fs->bit_pos = 0;

    switch (fs->state) {
    case FS_SYN2:
        if (byte == SYN) {
            fs->state = FS_SEEK_SOH;
        } else if (byte == SYN_INV) {
            fs->invert = !fs->invert;
            /* остаёмся в FS_SYN2, ждём следующий выровненный байт */
        } else {
            reset_to_wait_syn(fs);
        }
        break;

    case FS_SEEK_SOH:
        if (byte == SOH) {
            fs->state = FS_IN_FRAME;
            fs->buf_len = 0; /* SOH сюда НЕ пишем: BCS/CRC в реальном ACARS
                               * считается от Mode до ETX/ETB включительно,
                               * SOH (как и SYN SYN) - чисто служебный байт
                               * синхронизации, в контрольную сумму не входит */
            fs->got_suffix = 0;
            fs->suffix_crc_bytes_left = 0;
        } else {
            reset_to_wait_syn(fs);
        }
        break;

    case FS_IN_FRAME:
        if (fs->buf_len >= ACARS_MAX_PAYLOAD) {
            reset_to_wait_syn(fs);
            break;
        }
        fs->buf[fs->buf_len++] = byte;

        if (!fs->got_suffix) {
            uint8_t ch7 = byte & 0x7F;
            if (ch7 == ETX || ch7 == ETB) {
                fs->got_suffix = 1;
                fs->suffix_crc_bytes_left = 2;
            }
        } else {
            fs->suffix_crc_bytes_left--;
            if (fs->suffix_crc_bytes_left <= 0) {
                finalize_frame(fs);
                reset_to_wait_syn(fs);
            }
        }
        break;

    default:
        break;
    }
}
