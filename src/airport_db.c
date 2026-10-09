#include "airport_db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define HASH_BUCKETS 65536

typedef struct HashNode {
    AirportInfo info;
    struct HashNode *next;
} HashNode;

static HashNode *g_buckets[HASH_BUCKETS];
static int g_count = 0;
static int g_loaded = 0;
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

static unsigned hash_str_ci(const char *s) {
    unsigned h = 2166136261u; /* FNV-1a */
    for (; *s; s++) {
        h ^= (unsigned char)toupper((unsigned char)*s);
        h *= 16777619u;
    }
    return h;
}

static void safe_copy(char *dst, size_t dst_size, const char *src) {
    size_t i = 0;
    for (; i < dst_size - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Тот же разбор CSV по правилам RFC 4180 (кавычки, удвоенные кавычки
 * внутри поля), что и в aircraft_db.c - см. подробный комментарий там. */
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
                p++;
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

void airport_db_load(void) {
    clear_db();
    g_last_warning[0] = '\0';

    FILE *f = fopen("airport_db.csv", "r");
    if (!f)
        return; /* файла нет - необязательная функция, молча пропускаем */

    char line[4096];
    if (!fgets(line, sizeof(line), f)) {
        snprintf(g_last_warning, sizeof(g_last_warning),
                 "airport_db.csv найден, но пуст - обогащение аэропортов не будет работать");
        fclose(f);
        return;
    }

    /* Определяем номера нужных колонок по строке заголовка - надёжнее
     * жёстко заданных позиций, если формат OurAirports слегка
     * изменится. */
    int idx_ident = -1, idx_icao = -1, idx_name = -1, idx_muni = -1, idx_country = -1, idx_lat = -1, idx_lon = -1;
    {
        const char *p = line;
        int col = 0;
        char field[64];
        while (*p && *p != '\r' && *p != '\n') {
            p = parse_csv_field(p, field, sizeof(field));
            if (str_ci_eq(field, "ident"))
                idx_ident = col;
            else if (str_ci_eq(field, "icao_code"))
                idx_icao = col;
            else if (str_ci_eq(field, "name"))
                idx_name = col;
            else if (str_ci_eq(field, "municipality"))
                idx_muni = col;
            else if (str_ci_eq(field, "iso_country"))
                idx_country = col;
            else if (str_ci_eq(field, "latitude_deg"))
                idx_lat = col;
            else if (str_ci_eq(field, "longitude_deg"))
                idx_lon = col;
            col++;
        }
    }
    if (idx_ident < 0) {
        char preview[121];
        size_t plen = strcspn(line, "\r\n");
        if (plen > 120)
            plen = 120;
        memcpy(preview, line, plen);
        preview[plen] = '\0';
        snprintf(g_last_warning, sizeof(g_last_warning),
                 "airport_db.csv найден, но в заголовке не нашлась колонка \"ident\" - "
                 "формат не похож на ожидаемый (OurAirports). Заголовок файла: %s", preview);
        fclose(f);
        return;
    }

    while (fgets(line, sizeof(line), f)) {
        const char *p = line;
        int col = 0;
        char field[256];
        char ident[16] = "", icao_alt[16] = "", name[192] = "", muni[128] = "", country[16] = "";
        char slat[32] = "", slon[32] = "";

        while (*p && *p != '\r' && *p != '\n') {
            p = parse_csv_field(p, field, sizeof(field));
            if (col == idx_ident)
                safe_copy(ident, sizeof(ident), field);
            else if (col == idx_icao)
                safe_copy(icao_alt, sizeof(icao_alt), field);
            else if (col == idx_name)
                safe_copy(name, sizeof(name), field);
            else if (col == idx_muni)
                safe_copy(muni, sizeof(muni), field);
            else if (col == idx_country)
                safe_copy(country, sizeof(country), field);
            else if (col == idx_lat)
                safe_copy(slat, sizeof(slat), field);
            else if (col == idx_lon)
                safe_copy(slon, sizeof(slon), field);
            col++;
        }

        /* Используем ident как основной код (по документации OurAirports
         * это ICAO-код, если он у аэропорта есть) - если ident выглядит
         * пустым или явно не 4-буквенным ICAO-кодом (например, локальный
         * код без ICAO), пробуем отдельную колонку icao_code как запасной
         * вариант. Записи вообще без ICAO-кода (только IATA/локальный)
         * пропускаем - нам нужны именно 4-буквенные коды, которые и
         * встречаются в текстах ACARS-сообщений. */
        const char *code = ident;
        if (strlen(code) != 4)
            code = icao_alt;
        if (strlen(code) != 4)
            continue;
        int all_alpha = 1;
        for (int i = 0; i < 4; i++)
            if (!isalpha((unsigned char)code[i]))
                all_alpha = 0;
        if (!all_alpha)
            continue;

        HashNode *node = (HashNode *)malloc(sizeof(HashNode));
        if (!node)
            continue;
        memset(&node->info, 0, sizeof(node->info));
        safe_copy(node->info.icao, sizeof(node->info.icao), code);
        safe_copy(node->info.name, sizeof(node->info.name), name);
        safe_copy(node->info.municipality, sizeof(node->info.municipality), muni);
        safe_copy(node->info.country, sizeof(node->info.country), country);
        if (slat[0] && slon[0]) {
            node->info.lat = atof(slat);
            node->info.lon = atof(slon);
            node->info.has_coord = 1;
        }

        unsigned h = hash_str_ci(code) % HASH_BUCKETS;
        node->next = g_buckets[h];
        g_buckets[h] = node;
        g_count++;
    }

    fclose(f);
    g_loaded = 1;
    if (g_count == 0) {
        snprintf(g_last_warning, sizeof(g_last_warning),
                 "airport_db.csv найден, заголовок распознан, но нет ни одной строки с "
                 "валидным 4-буквенным ICAO-кодом - проверьте содержимое файла.");
    }
}

const char *airport_db_last_warning(void) {
    return g_last_warning[0] ? g_last_warning : NULL;
}

/* Небольшая встроенная таблица аэропортов, которые встречаются в журналах чаще всего: чтобы маршрут показывался
 * (с названием и положением на карте) даже без файла airport_db.csv. Если файл есть - его данные главнее;
 * из таблицы берутся только отсутствующие в нём аэропорты и координаты. Координаты - КТА аэропорта, ~1 км. */
typedef struct { const char *icao, *city, *name; double lat, lon; } BuiltinApt;
static const BuiltinApt kBuiltin[] = {
    {"UUEE", "Москва", "Шереметьево", 55.9726, 37.4146}, {"UUDD", "Москва", "Домодедово", 55.4088, 37.9063},
    {"UUWW", "Москва", "Внуково", 55.5915, 37.2615},     {"UUBW", "Жуковский", "Жуковский", 55.5533, 38.1500},
    {"ULLI", "Санкт-Петербург", "Пулково", 59.8003, 30.2625}, {"URWW", "Волгоград", "Гумрак", 48.7825, 44.3455},
    {"UWGG", "Нижний Новгород", "Стригино", 56.2301, 43.7840}, {"URSS", "Сочи", "Адлер", 43.4499, 39.9566},
    {"URKK", "Краснодар", "Пашковский", 45.0347, 39.1705}, {"UWWW", "Самара", "Курумоч", 53.5049, 50.1643},
    {"UWKD", "Казань", "Казань", 55.6062, 49.2787},        {"USSS", "Екатеринбург", "Кольцово", 56.7430, 60.8027},
    {"UNOO", "Омск", "Центральный", 54.9670, 73.3105},     {"UNNT", "Новосибирск", "Толмачёво", 55.0126, 82.6507},
    {"UIII", "Иркутск", "Иркутск", 52.2680, 104.3890},     {"UHWW", "Владивосток", "Кневичи", 43.3989, 132.1480},
    {"UHHH", "Хабаровск", "Новый", 48.5280, 135.1880},     {"USRR", "Сургут", "Сургут", 61.3437, 73.4018},
    {"UUOO", "Воронеж", "Чертовицкое", 51.8142, 39.2296},  {"USII", "Ижевск", "Ижевск", 56.8281, 53.4575},
    {"UWOR", "Оренбург", "Центральный", 51.7958, 55.4567}, {"URWA", "Астрахань", "Нариманово", 46.2833, 48.0063},
    {"UMMS", "Минск", "Минск-2", 53.8825, 28.0307},       {"UTTT", "Ташкент", "Ислам Каримов", 41.2579, 69.2812},
    {"UAAA", "Алма-Ата", "Алма-Ата", 43.3521, 77.0405},    {"UACC", "Астана", "Нурсултан Назарбаев", 51.0222, 71.4669},
    {"UATE", "Актау", "Актау", 43.8601, 51.0919},         {"UDYZ", "Ереван", "Звартноц", 40.1473, 44.3959},
    {"UGTB", "Тбилиси", "Тбилиси", 41.6692, 44.9547},     {"UBBB", "Баку", "Гейдар Алиев", 40.4675, 50.0467},
    {"LTFM", "Стамбул", "Стамбул", 41.2753, 28.7519},     {"LTAI", "Анталья", "Анталья", 36.8987, 30.8005},
    {"OMDB", "Дубай", "Дубай", 25.2528, 55.3644},         {"HECA", "Каир", "Каир", 30.1219, 31.4056},
    {"HEGN", "Хургада", "Хургада", 27.1783, 33.7994},     {"HESH", "Шарм-эль-Шейх", "Шарм-эль-Шейх", 27.9773, 34.3950},
};

static const BuiltinApt *builtin_find(const char *icao) {
    for (size_t i = 0; i < sizeof kBuiltin / sizeof *kBuiltin; i++)
        if (str_ci_eq(kBuiltin[i].icao, icao))
            return &kBuiltin[i];
    return NULL;
}

int airport_db_lookup(const char *icao, AirportInfo *out) {
    if (!icao || strlen(icao) != 4)
        return 0;
    if (g_loaded) {
        unsigned h = hash_str_ci(icao) % HASH_BUCKETS;
        for (HashNode *n = g_buckets[h]; n; n = n->next) {
            if (str_ci_eq(n->info.icao, icao)) {
                *out = n->info;
                if (!out->has_coord) {
                    const BuiltinApt *b = builtin_find(icao);
                    if (b) { out->lat = b->lat; out->lon = b->lon; out->has_coord = 1; }
                }
                return 1;
            }
        }
    }
    const BuiltinApt *b = builtin_find(icao);
    if (!b)
        return 0;
    memset(out, 0, sizeof *out);
    for (int i = 0; i < 4; i++)
        out->icao[i] = (char)toupper((unsigned char)icao[i]);
    snprintf(out->name, sizeof out->name, "%s", b->name);
    snprintf(out->municipality, sizeof out->municipality, "%s", b->city);
    out->lat = b->lat;
    out->lon = b->lon;
    out->has_coord = 1;
    return 1;
}

int airport_db_map_list(AirportInfo *out, int max) {
    int n = 0;
    for (size_t i = 0; i < sizeof kBuiltin / sizeof *kBuiltin && n < max; i++)
        if (airport_db_lookup(kBuiltin[i].icao, &out[n]) && out[n].has_coord)
            n++;
    return n;
}

int airport_db_count(void) {
    return g_count;
}

int airport_db_scan_pair(const char *text, AirportInfo *out_first, AirportInfo *out_second) {
    if (!g_loaded || !text)
        return 0;
    size_t len = strlen(text);
    for (size_t i = 0; i + 8 <= len; i++) {
        int ok = 1;
        for (int k = 0; k < 8; k++)
            if (!isupper((unsigned char)text[i + k])) {
                ok = 0;
                break;
            }
        if (!ok)
            continue;
        char code1[5], code2[5];
        memcpy(code1, text + i, 4);
        code1[4] = '\0';
        memcpy(code2, text + i + 4, 4);
        code2[4] = '\0';
        if (airport_db_lookup(code1, out_first) && airport_db_lookup(code2, out_second))
            return 1;
    }
    return 0;
}
