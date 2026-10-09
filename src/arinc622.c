#include "arinc622.h"
#include <string.h>
#include <ctype.h>

/* Известные коды IMI (Imbedded Message Identifier) - по исходникам
 * проекта libacars (see include/arinc622.h для ссылок). Список не
 * претендует на полноту - libacars, вероятно, знает больше кодов, но
 * эти подтверждены по нескольким независимым примерам. */
static const struct {
    const char *imi;
    const char *desc;
} kKnownImi[] = {
    {"AT1", "FANS-1/A CPDLC: сообщение"},
    {"CR1", "FANS-1/A CPDLC: запрос соединения (Connect Request)"},
    {"CC1", "FANS-1/A CPDLC: подтверждение соединения (Connect Confirm)"},
    {"DR1", "FANS-1/A CPDLC: запрос разъединения (Disconnect Request)"},
    {"ADS", "ADS-C: сообщение (слежение за позицией по контракту)"},
    {"DIS", "ADS-C: запрос разъединения"},
};
#define N_KNOWN_IMI (int)(sizeof(kKnownImi) / sizeof(kKnownImi[0]))

static const char *imi_lookup(const char *imi) {
    for (int i = 0; i < N_KNOWN_IMI; i++)
        if (strcmp(imi, kKnownImi[i].imi) == 0)
            return kKnownImi[i].desc;
    return "неизвестный тип (IMI не из известного списка)";
}

static int is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

int arinc622_parse(const char *text, const char *acars_addr, Arinc622Envelope *out) {
    memset(out, 0, sizeof(*out));
    if (!text || text[0] != '/')
        return 0;

    const char *p = text + 1;
    const char *dot1 = strchr(p, '.');
    if (!dot1)
        return 0;
    const char *dot2 = strchr(dot1 + 1, '.');
    if (!dot2)
        return 0;

    size_t gs_len = (size_t)(dot1 - p);
    size_t imi_len = (size_t)(dot2 - (dot1 + 1));
    if (gs_len == 0 || gs_len >= sizeof(out->ground_station))
        return 0;
    if (imi_len == 0 || imi_len >= sizeof(out->imi))
        return 0;

    const char *rest = dot2 + 1;
    size_t rest_len = strlen(rest);

    /* Граница "борт / hex-данные" в самом конверте никак не помечена -
     * определяем её по уже известной регистрации борта из заголовка
     * ACARS-сообщения (acars_addr), а не гадаем. ACARS-заголовок
     * обычно хранит adres с ведущей точкой (".F-GSPK") - её пропускаем. */
    if (!acars_addr)
        return 0;
    const char *reg = acars_addr;
    if (reg[0] == '.')
        reg++;
    size_t reg_len = strlen(reg);
    if (reg_len == 0 || reg_len >= sizeof(out->aircraft_reg))
        return 0;
    if (rest_len < reg_len)
        return 0;
    for (size_t i = 0; i < reg_len; i++) {
        if (toupper((unsigned char)rest[i]) != toupper((unsigned char)reg[i]))
            return 0; /* не похоже на конверт с этим бортом - не гадаем дальше */
    }

    const char *after_reg = rest + reg_len;
    size_t after_len = rest_len - reg_len;
    if (after_len < 4)
        return 0; /* меньше 4 символов не хватит даже на одну CRC */

    size_t hex_len = after_len - 4;
    const char *hex_start = after_reg;
    const char *crc_start = after_reg + hex_len;

    /* hex-данные должны состоять ИСКЛЮЧИТЕЛЬНО из hex-цифр - если нет,
     * это, скорее всего, не конверт ARINC 622, а случайно похожий на
     * него текст (например, обычное сообщение, в котором волей случая
     * тоже есть косая черта и две точки) - не выдаём его за конверт. */
    for (size_t i = 0; i < hex_len; i++)
        if (!is_hex_digit(hex_start[i]))
            return 0;
    for (int i = 0; i < 4; i++)
        if (!is_hex_digit(crc_start[i]))
            return 0;

    if (hex_len >= sizeof(out->hex_payload))
        hex_len = sizeof(out->hex_payload) - 1; /* подстраховка, не должно случаться
                                                  * при реальных размерах ACARS-кадра */

    memcpy(out->ground_station, p, gs_len);
    out->ground_station[gs_len] = '\0';
    memcpy(out->imi, dot1 + 1, imi_len);
    out->imi[imi_len] = '\0';
    out->imi_description = imi_lookup(out->imi);
    memcpy(out->aircraft_reg, rest, reg_len);
    out->aircraft_reg[reg_len] = '\0';
    memcpy(out->hex_payload, hex_start, hex_len);
    out->hex_payload[hex_len] = '\0';
    out->hex_payload_len = (int)hex_len;
    memcpy(out->inner_crc, crc_start, 4);
    out->inner_crc[4] = '\0';
    out->valid = 1;
    return 1;
}
