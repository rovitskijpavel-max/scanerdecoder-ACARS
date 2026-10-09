#include "aircraft_db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define HASH_BUCKETS 32768

typedef struct HashNode {
    AircraftInfo info;
    char key[16]; /* регистрация только из букв и цифр, в верхнем регистре - по ней идёт поиск */
    struct HashNode *next;
} HashNode;

static HashNode *g_buckets[HASH_BUCKETS];
static int g_count = 0;
static int g_loaded = 0;
/* Диагностическое сообщение о последней попытке загрузки (если она не
 * удалась содержательно - файл найден, но не распознан/пуст). NULL,
 * если всё прошло гладко или файла не было вовсе (тогда сообщать
 * нечего - это ожидаемый, "тихий" случай для тех, кто эту
 * необязательную функцию не использует). Печатать его должен вызывающий
 * код - см. aircraft_db_last_warning() - а не сама эта функция: если
 * печатать прямо тут, в GUI-режиме сообщение уйдёт в консоль ДО того,
 * как run_gui() успеет перенаправить вывод в окно, и потеряется (та же
 * ловушка, что уже была с сообщением об успешной загрузке). */
static char g_last_warning[512] = "";

static int str_ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Приводит регистрацию к ключу поиска: только буквы и цифры, верхний регистр. Без этого
 * "RA73104" из ACARS (дефис в 7-символьное поле адреса не влезает) не находит "RA-73104" из
 * базы OpenSky, а ".VP-BSY" с ведущей точкой и "vp-bsy" в другом регистре - тоже. */
static void make_key(const char *reg, char *key, size_t cap) {
    size_t o = 0;
    for (; *reg && o + 1 < cap; reg++)
        if (isalnum((unsigned char)*reg))
            key[o++] = (char)toupper((unsigned char)*reg);
    key[o] = '\0';
}

static unsigned hash_str_ci(const char *s) {
    unsigned h = 2166136261u; /* FNV-1a */
    for (; *s; s++) {
        h ^= (unsigned char)toupper((unsigned char)*s);
        h *= 16777619u;
    }
    return h;
}

/* Разбирает одно CSV-поле по правилам RFC 4180 (кавычки, удвоенные
 * кавычки внутри поля как экранирование) начиная с *p, копирует его
 * (без внешних кавычек) в out и возвращает указатель на символ сразу
 * после поля (запятая пропускается, если она там была). */
static const char *parse_csv_field(const char *p, char *out, size_t out_size) {
    size_t o = 0;
    if (*p == '"') {
        p++;
        while (*p) {
            if (*p == '"') {
                if (p[1] == '"') {
                    if (o < out_size - 1)
                        out[o++] = '"';
                    p += 2;
                    continue;
                }
                p++; /* закрывающая кавычка */
                break;
            }
            if (o < out_size - 1)
                out[o++] = *p;
            p++;
        }
        while (*p && *p != ',' && *p != '\r' && *p != '\n')
            p++;
    } else {
        while (*p && *p != ',' && *p != '\r' && *p != '\n') {
            if (o < out_size - 1)
                out[o++] = *p;
            p++;
        }
    }
    out[o] = '\0';
    if (*p == ',')
        p++;
    return p;
}

static void safe_copy(char *dst, size_t dst_size, const char *src) {
    size_t i = 0;
    for (; i < dst_size - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static void clear_db(void) {
    for (int i = 0; i < HASH_BUCKETS; i++) {
        HashNode *n = g_buckets[i];
        while (n) {
            HashNode *next = n->next;
            free(n);
            n = next;
        }
        g_buckets[i] = NULL;
    }
    g_count = 0;
    g_loaded = 0;
}

void aircraft_db_load(void) {
    clear_db();
    g_last_warning[0] = '\0';

    FILE *f = fopen("aircraft_db.csv", "r");
    if (!f)
        return; /* файла нет - необязательная функция, молча пропускаем */

    char line[4096];
    if (!fgets(line, sizeof(line), f)) {
        snprintf(g_last_warning, sizeof(g_last_warning),
                 "aircraft_db.csv найден, но пуст - обогащение бортов не будет работать");
        fclose(f);
        return;
    }

    /* Определяем номера нужных колонок по строке заголовка - не
     * полагаемся на жёстко заданные позиции, так надёжнее, если в
     * будущем формат файла OpenSky слегка изменится (появятся/пропадут
     * колонки). */
    int idx_reg = -1, idx_icao24 = -1, idx_mfr_name = -1, idx_mfr_icao = -1;
    int idx_model = -1, idx_typecode = -1, idx_operator = -1;
    {
        const char *p = line;
        int col = 0;
        char field[64];
        while (*p && *p != '\r' && *p != '\n') {
            p = parse_csv_field(p, field, sizeof(field));
            if (str_ci_eq(field, "registration"))
                idx_reg = col;
            else if (str_ci_eq(field, "icao24"))
                idx_icao24 = col;
            else if (str_ci_eq(field, "manufacturername"))
                idx_mfr_name = col;
            else if (str_ci_eq(field, "manufacturericao"))
                idx_mfr_icao = col;
            else if (str_ci_eq(field, "model"))
                idx_model = col;
            else if (str_ci_eq(field, "typecode"))
                idx_typecode = col;
            else if (str_ci_eq(field, "operator"))
                idx_operator = col;
            col++;
        }
    }
    if (idx_reg < 0) {
        /* Заголовок не похож на ожидаемый формат OpenSky - не гадаем,
         * какая колонка тут за что отвечает. Раньше здесь был тихий
         * отказ без единого сообщения - та же ошибка, что уже не раз
         * встречалась в этом проекте (молчаливый отказ вместо понятной
         * диагностики). Печатаем начало реальной строки заголовка -
         * пригодится, чтобы понять, чем формат отличается от ожидаемого. */
        char preview[121];
        size_t plen = strcspn(line, "\r\n");
        if (plen > 120) plen = 120;
        memcpy(preview, line, plen);
        preview[plen] = '\0';
        snprintf(g_last_warning, sizeof(g_last_warning),
                 "aircraft_db.csv найден, но в заголовке не нашлась колонка \"registration\" - "
                 "формат не похож на ожидаемый (OpenSky Network). Заголовок файла: %s", preview);
        fclose(f);
        return;
    }

    while (fgets(line, sizeof(line), f)) {
        const char *p = line;
        int col = 0;
        char field[256];
        char reg[16] = "", icao24[8] = "", mfr_name[64] = "", mfr_icao[64] = "";
        char model[64] = "", typecode[16] = "", operator_name[128] = "";

        while (*p && *p != '\r' && *p != '\n') {
            p = parse_csv_field(p, field, sizeof(field));
            if (col == idx_reg)
                safe_copy(reg, sizeof(reg), field);
            else if (col == idx_icao24)
                safe_copy(icao24, sizeof(icao24), field);
            else if (col == idx_mfr_name)
                safe_copy(mfr_name, sizeof(mfr_name), field);
            else if (col == idx_mfr_icao)
                safe_copy(mfr_icao, sizeof(mfr_icao), field);
            else if (col == idx_model)
                safe_copy(model, sizeof(model), field);
            else if (col == idx_typecode)
                safe_copy(typecode, sizeof(typecode), field);
            else if (col == idx_operator)
                safe_copy(operator_name, sizeof(operator_name), field);
            col++;
        }

        if (reg[0] == '\0')
            continue; /* без регистрации запись бесполезна для нашего поиска */

        HashNode *node = (HashNode *)malloc(sizeof(HashNode));
        if (!node)
            continue; /* не хватило памяти - пропускаем эту запись, не падаем */
        memset(&node->info, 0, sizeof(node->info));
        safe_copy(node->info.registration, sizeof(node->info.registration), reg);
        safe_copy(node->info.icao24, sizeof(node->info.icao24), icao24);
        safe_copy(node->info.manufacturer, sizeof(node->info.manufacturer), mfr_name[0] ? mfr_name : mfr_icao);
        safe_copy(node->info.model, sizeof(node->info.model), model);
        safe_copy(node->info.typecode, sizeof(node->info.typecode), typecode);
        safe_copy(node->info.operator_name, sizeof(node->info.operator_name), operator_name);

        make_key(reg, node->key, sizeof(node->key));
        if (node->key[0] == '\0') { /* в "регистрации" не оказалось ни одной буквы/цифры */
            free(node);
            continue;
        }
        unsigned h = hash_str_ci(node->key) % HASH_BUCKETS;
        node->next = g_buckets[h];
        g_buckets[h] = node;
        g_count++;
    }

    fclose(f);
    g_loaded = 1;
    if (g_count == 0) {
        snprintf(g_last_warning, sizeof(g_last_warning),
                 "aircraft_db.csv найден, заголовок распознан, но нет ни одной строки "
                 "с непустой регистрацией - проверьте, что после заголовка есть данные.");
    }
}

const char *aircraft_db_last_warning(void) {
    return g_last_warning[0] ? g_last_warning : NULL;
}

int aircraft_db_lookup(const char *registration, AircraftInfo *out) {
    if (!g_loaded || !registration)
        return 0;
    char key[16];
    make_key(registration, key, sizeof(key)); /* ведущая точка, дефис, регистр - не важны */
    if (!key[0])
        return 0;

    unsigned h = hash_str_ci(key) % HASH_BUCKETS;
    for (HashNode *n = g_buckets[h]; n; n = n->next) {
        if (strcmp(n->key, key) == 0) {
            *out = n->info;
            return 1;
        }
    }
    return 0;
}

int aircraft_db_count(void) {
    return g_count;
}
