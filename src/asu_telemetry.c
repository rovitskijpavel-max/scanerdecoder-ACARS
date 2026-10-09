#include "asu_telemetry.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "units.h"

static void trim_copy(char *dst, size_t dst_size, const char *src, size_t len) {
    while (len > 0 && (src[0] == ' ')) {
        src++;
        len--;
    }
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\r' || src[len - 1] == '\n'))
        len--;
    size_t n = len < dst_size - 1 ? len : dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Если строка (длиной line_len) начинается ровно с key - копирует
 * остаток строки (после key, обрезав пробелы) в dst и возвращает 1;
 * иначе не трогает dst и возвращает 0. */
static int try_key(const char *line, size_t line_len, const char *key,
                    char *dst, size_t dst_size) {
    size_t klen = strlen(key);
    if (line_len < klen)
        return 0;
    if (strncmp(line, key, klen) != 0)
        return 0;
    trim_copy(dst, dst_size, line + klen, line_len - klen);
    return 1;
}

int asu_telemetry_parse(const char *text, AsuTelemetry *out) {
    memset(out, 0, sizeof(*out));
    if (!text)
        return 0;

    const char *p = text;
    while (*p) {
        size_t line_len = strcspn(p, "\r\n");
        if (line_len > 0) {
            char dir_letter = 0;
            char lat_val[24] = "", lon_val[24] = "";

            if (try_key(p, line_len, "FUEL", out->fuel, sizeof(out->fuel)))
                out->valid = 1;
            else if (try_key(p, line_len, "TEMP", out->temp, sizeof(out->temp)))
                out->valid = 1;
            else if (try_key(p, line_len, "WDIR", out->wind_dir, sizeof(out->wind_dir)))
                out->valid = 1;
            else if (try_key(p, line_len, "WSPD", out->wind_speed, sizeof(out->wind_speed)))
                out->valid = 1;
            else if (try_key(p, line_len, "ETA", out->eta, sizeof(out->eta)))
                out->valid = 1;
            else if (try_key(p, line_len, "ALT", out->alt, sizeof(out->alt)))
                out->valid = 1;
            else if (line_len >= 4 && strncmp(p, "LATN", 4) == 0) {
                dir_letter = 'N';
                trim_copy(lat_val, sizeof(lat_val), p + 4, line_len - 4);
            } else if (line_len >= 4 && strncmp(p, "LATS", 4) == 0) {
                dir_letter = 'S';
                trim_copy(lat_val, sizeof(lat_val), p + 4, line_len - 4);
            } else if (line_len >= 4 && strncmp(p, "LONE", 4) == 0) {
                dir_letter = 'E';
                trim_copy(lon_val, sizeof(lon_val), p + 4, line_len - 4);
            } else if (line_len >= 4 && strncmp(p, "LONW", 4) == 0) {
                dir_letter = 'W';
                trim_copy(lon_val, sizeof(lon_val), p + 4, line_len - 4);
            }

            if (dir_letter == 'N' || dir_letter == 'S') {
                snprintf(out->lat, sizeof(out->lat), "%c %.20s", dir_letter, lat_val);
                out->valid = 1;
            } else if (dir_letter == 'E' || dir_letter == 'W') {
                snprintf(out->lon, sizeof(out->lon), "%c %.20s", dir_letter, lon_val);
                out->valid = 1;
            }
        }
        p += line_len;
        while (*p == '\r' || *p == '\n')
            p++;
    }

    return out->valid;
}

void asu_telemetry_format_annotated(const char *text, char *out, size_t out_size) {
    static const struct {
        const char *key;
        const char *ru;
    } table[] = {
        {"FUEL", "Топливо"},   {"TEMP", "Температура"}, {"WDIR", "Направление ветра"},
        {"WSPD", "Скорость ветра"}, {"ETA", "Расчётное время прибытия"}, {"ALT", "Высота"},
        {"LATN", "Широта (N)"}, {"LATS", "Широта (S)"}, {"LONE", "Долгота (E)"},
        {"LONW", "Долгота (W)"}, {NULL, NULL}};

    size_t pos = 0;
    if (out_size)
        out[0] = '\0';
    const char *p = text;
    while (*p) {
        size_t line_len = strcspn(p, "\r\n");
        if (line_len > 0) {
            for (int k = 0; table[k].key; k++) {
                size_t klen = strlen(table[k].key);
                if (line_len >= klen && strncmp(p, table[k].key, klen) == 0) {
                    char raw_line[40];
                    size_t rl = line_len < sizeof(raw_line) - 1 ? line_len : sizeof(raw_line) - 1;
                    memcpy(raw_line, p, rl);
                    raw_line[rl] = '\0';
                    char value[64];
                    trim_copy(value, sizeof(value), p + klen, line_len - klen);
                    if (strcmp(table[k].key, "ALT") == 0) {
                        char shown[48];
                        alt_ft_text(value, shown, sizeof(shown), NULL);
                        snprintf(value, sizeof(value), "%s", shown);
                    }
                    int n = snprintf(out + pos, out_size > pos ? out_size - pos : 0,
                                      "  \"%s\"  ->  %s: %s\n", raw_line, table[k].ru, value);
                    if (n < 0 || pos + (size_t)n >= out_size)
                        return;
                    pos += (size_t)n;
                    break;
                }
            }
        }
        p += line_len;
        while (*p == '\r' || *p == '\n')
            p++;
    }
}
