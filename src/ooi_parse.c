#include "ooi_parse.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static int is_known_key(const char *key) {
    static const char *known[] = {"AN", "DA", "DS", "AD", "OT", "OUT", "OF", "OFF",
                                   "ON", "IN", "FB", "BF", "DC", "LA", "LR"};
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (strcmp(key, known[i]) == 0)
            return 1;
    return 0;
}

static void copy_trim(char *dst, size_t dst_size, const char *src, size_t len) {
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\r' || src[len - 1] == '\n'))
        len--;
    size_t n = len < dst_size - 1 ? len : dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int ooi_parse(const char *text, OoiInfo *out) {
    memset(out, 0, sizeof(*out));
    if (!text)
        return 0;

    const char *fi = strstr(text, "FI ");
    if (!fi)
        return 0;
    fi += 3;

    const char *slash = strchr(fi, '/');
    if (!slash)
        return 0; /* без единой пары ключ/значение после FI - не похоже на этот формат,
                   * не гадаем (иначе слово вроде "FIRST" в обычном тексте могло бы
                   * ложно сойти за начало FI-сообщения) */

    /* Первый же ключ после номера рейса обязан быть из известного
     * списка - иначе это, скорее всего, случайное совпадение "FI " в
     * обычном тексте, а не настоящее FI-сообщение. */
    const char *p = slash + 1;
    const char *sp = strchr(p, ' ');
    if (!sp)
        return 0;
    size_t klen0 = (size_t)(sp - p);
    if (klen0 == 0 || klen0 >= 8)
        return 0;
    char key0[8];
    memcpy(key0, p, klen0);
    key0[klen0] = '\0';
    if (!is_known_key(key0))
        return 0;

    copy_trim(out->flight_id, sizeof(out->flight_id), fi, (size_t)(slash - fi));

    p = slash;
    int found_route_field = 0;
    while (p && *p == '/') {
        p++;
        const char *space = strchr(p, ' ');
        const char *next_slash_early = strchr(p, '/');
        if (!space || (next_slash_early && next_slash_early < space))
            break; /* ключ без пробела перед значением - формат не совпадает, прекращаем */

        size_t klen = (size_t)(space - p);
        if (klen == 0 || klen >= 8)
            break;
        char key[8];
        memcpy(key, p, klen);
        key[klen] = '\0';

        const char *val_start = space + 1;
        const char *next_slash = strchr(val_start, '/');
        size_t vlen = next_slash ? (size_t)(next_slash - val_start) : strlen(val_start);
        for (size_t i = 0; i < vlen; i++) {
            if (val_start[i] == '\n' || val_start[i] == '\r') {
                vlen = i;
                break;
            }
        }

        if (strcmp(key, "DA") == 0) {
            copy_trim(out->departure, sizeof(out->departure), val_start, vlen);
            found_route_field = 1;
        } else if (strcmp(key, "DS") == 0) {
            copy_trim(out->destination, sizeof(out->destination), val_start, vlen);
            found_route_field = 1;
        } else if (strcmp(key, "AD") == 0) {
            copy_trim(out->arrival, sizeof(out->arrival), val_start, vlen);
            found_route_field = 1;
        } else if (strcmp(key, "OT") == 0 || strcmp(key, "OUT") == 0) {
            copy_trim(out->out_time, sizeof(out->out_time), val_start, vlen);
        } else if (strcmp(key, "OF") == 0 || strcmp(key, "OFF") == 0) {
            copy_trim(out->off_time, sizeof(out->off_time), val_start, vlen);
        } else if (strcmp(key, "ON") == 0) {
            copy_trim(out->on_time, sizeof(out->on_time), val_start, vlen);
        } else if (strcmp(key, "IN") == 0) {
            copy_trim(out->in_time, sizeof(out->in_time), val_start, vlen);
        }
        /* остальные известные ключи (AN/FB/BF/DC/LA/LR) - не связаны с
         * маршрутом, пропускаем, нам они здесь не нужны */

        p = next_slash;
    }

    out->valid = found_route_field;
    return out->valid;
}

void ooi_format_annotated(const char *text, char *out, size_t out_size) {
    size_t pos = 0;
    if (out_size)
        out[0] = '\0';

    const char *fi = strstr(text, "FI ");
    if (!fi)
        return;
    fi += 3;
    const char *slash = strchr(fi, '/');
    if (!slash)
        return;

    char flight_id[16];
    copy_trim(flight_id, sizeof(flight_id), fi, (size_t)(slash - fi));
    {
        char raw[24];
        size_t rl = (size_t)(slash - fi) < sizeof(raw) - 1 ? (size_t)(slash - fi) : sizeof(raw) - 1;
        memcpy(raw, fi, rl);
        raw[rl] = '\0';
        int n = snprintf(out + pos, out_size > pos ? out_size - pos : 0,
                          "  \"%s\"  ->  Номер рейса: %s\n", raw, flight_id);
        if (n < 0 || pos + (size_t)n >= out_size)
            return;
        pos += (size_t)n;
    }

    static const struct {
        const char *key;
        const char *ru;
    } table[] = {{"DA", "Аэропорт вылета"}, {"DS", "Аэропорт назначения"},
                 {"AD", "Аэропорт (прибытие)"}, {"OT", "Время OUT (ушёл от гейта)"},
                 {"OUT", "Время OUT (ушёл от гейта)"}, {"OF", "Время OFF (взлёт)"},
                 {"OFF", "Время OFF (взлёт)"}, {"ON", "Время ON (посадка)"},
                 {"IN", "Время IN (прибыл на гейт)"}, {NULL, NULL}};

    const char *p = slash;
    while (p && *p == '/') {
        p++;
        const char *space = strchr(p, ' ');
        const char *next_slash_early = strchr(p, '/');
        if (!space || (next_slash_early && next_slash_early < space))
            break;

        size_t klen = (size_t)(space - p);
        if (klen == 0 || klen >= 8)
            break;
        char key[8];
        memcpy(key, p, klen);
        key[klen] = '\0';

        const char *val_start = space + 1;
        const char *next_slash = strchr(val_start, '/');
        size_t vlen = next_slash ? (size_t)(next_slash - val_start) : strlen(val_start);
        for (size_t i = 0; i < vlen; i++)
            if (val_start[i] == '\n' || val_start[i] == '\r') {
                vlen = i;
                break;
            }

        for (int k = 0; table[k].key; k++) {
            if (strcmp(key, table[k].key) == 0) {
                char value[24];
                copy_trim(value, sizeof(value), val_start, vlen);
                char raw[40];
                snprintf(raw, sizeof(raw), "/%s %.*s", key, (int)vlen, val_start);
                int n = snprintf(out + pos, out_size > pos ? out_size - pos : 0,
                                  "  \"%s\"  ->  %s: %s\n", raw, table[k].ru, value);
                if (n < 0 || pos + (size_t)n >= out_size)
                    return;
                pos += (size_t)n;
                break;
            }
        }

        p = next_slash;
    }
}
