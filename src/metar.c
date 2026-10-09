#include "metar.h"
#include "airport_db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int all_upper_alpha(const char *s, int len) {
    for (int i = 0; i < len; i++)
        if (!isupper((unsigned char)s[i]))
            return 0;
    return 1;
}

static int all_digit(const char *s, int len) {
    for (int i = 0; i < len; i++)
        if (!isdigit((unsigned char)s[i]))
            return 0;
    return 1;
}

static void safe_copy(char *dst, size_t dst_size, const char *src) {
    size_t i = 0;
    for (; i < dst_size - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Стандартные (ICAO Annex 3 / WMO 782) двухбуквенные коды явлений
 * погоды - используются как "кирпичики": токен вида "-RA", "+TSRA",
 * "VCSH" и т.п. составлен из последовательности таких кодов, иногда с
 * префиксом интенсивности + / -. */
static const char *kPhenomenonCodes[] = {
    "MI", "BC", "PR", "DR", "BL", "SH", "TS", "FZ", /* описатели */
    "DZ", "RA", "SN", "SG", "IC", "PL", "GR", "GS", "UP", /* осадки */
    "BR", "FG", "FU", "VA", "DU", "SA", "HZ", /* ухудшающие видимость */
    "PO", "SQ", "FC", "SS", "DS", /* прочие */
    "VC", "RE", /* в окрестности / недавние */
    NULL};

static int is_phenomenon_token(const char *tok) {
    if (*tok == '+' || *tok == '-')
        tok++;
    size_t len = strlen(tok);
    if (len == 0 || len % 2 != 0 || len > 10)
        return 0;
    for (size_t i = 0; i < len; i += 2) {
        char code[3] = {tok[i], tok[i + 1], '\0'};
        int found = 0;
        for (int k = 0; kPhenomenonCodes[k]; k++)
            if (strcmp(code, kPhenomenonCodes[k]) == 0) {
                found = 1;
                break;
            }
        if (!found)
            return 0;
    }
    return 1;
}

static int is_cloud_token(const char *tok, MetarCloud *out) {
    if (strcmp(tok, "NSC") == 0 || strcmp(tok, "NCD") == 0 || strcmp(tok, "SKC") == 0) {
        safe_copy(out->type, sizeof(out->type), tok);
        out->height_ft = -1;
        return 1;
    }
    size_t len = strlen(tok);
    if (len != 6)
        return 0;
    char prefix[4] = {tok[0], tok[1], tok[2], '\0'};
    if (strcmp(prefix, "FEW") != 0 && strcmp(prefix, "SCT") != 0 &&
        strcmp(prefix, "BKN") != 0 && strcmp(prefix, "OVC") != 0)
        return 0;
    if (!all_digit(tok + 3, 3))
        return 0;
    safe_copy(out->type, sizeof(out->type), prefix);
    /* В METAR высота облачности задаётся в СОТНЯХ футов - "SCT035"
     * значит 035*100 = 3500 футов, а не 35. */
    out->height_ft = ((tok[3] - '0') * 100 + (tok[4] - '0') * 10 + (tok[5] - '0')) * 100;
    return 1;
}

static int is_wind_group(const char *tok, MetarReport *out) {
    size_t len = strlen(tok);
    int dir_variable = 0;
    int pos = 0;

    if (len >= 3 && strncmp(tok, "VRB", 3) == 0) {
        dir_variable = 1;
        pos = 3;
    } else if (len >= 3 && all_digit(tok, 3)) {
        pos = 3;
    } else {
        return 0;
    }

    int speed_start = pos;
    int speed_len = 0;
    while (pos < (int)len && isdigit((unsigned char)tok[pos])) {
        pos++;
        speed_len++;
    }
    if (speed_len < 2 || speed_len > 3)
        return 0;

    int gust = -1;
    if (pos < (int)len && tok[pos] == 'G') {
        pos++;
        int gust_start = pos;
        int gust_len = 0;
        while (pos < (int)len && isdigit((unsigned char)tok[pos])) {
            pos++;
            gust_len++;
        }
        if (gust_len < 2 || gust_len > 3)
            return 0;
        char gbuf[4];
        memcpy(gbuf, tok + gust_start, (size_t)gust_len);
        gbuf[gust_len] = '\0';
        gust = atoi(gbuf);
    }

    const char *unit = tok + pos;
    if (strcmp(unit, "KT") != 0 && strcmp(unit, "MPS") != 0 && strcmp(unit, "KMH") != 0)
        return 0;

    out->wind_variable = dir_variable;
    out->wind_dir = dir_variable ? -1 : (tok[0] - '0') * 100 + (tok[1] - '0') * 10 + (tok[2] - '0');
    char sbuf[4];
    memcpy(sbuf, tok + speed_start, (size_t)speed_len);
    sbuf[speed_len] = '\0';
    out->wind_speed = atoi(sbuf);
    out->wind_gust = gust;
    safe_copy(out->wind_unit, sizeof(out->wind_unit), unit);
    return 1;
}

static int is_wind_variable_group(const char *tok, MetarReport *out) {
    if (strlen(tok) != 7 || tok[3] != 'V')
        return 0;
    if (!all_digit(tok, 3) || !all_digit(tok + 4, 3))
        return 0;
    out->wind_var_from = (tok[0] - '0') * 100 + (tok[1] - '0') * 10 + (tok[2] - '0');
    out->wind_var_to = (tok[4] - '0') * 100 + (tok[5] - '0') * 10 + (tok[6] - '0');
    return 1;
}

static int is_temp_dewpoint(const char *tok, MetarReport *out) {
    const char *slash = strchr(tok, '/');
    if (!slash)
        return 0;
    const char *t = tok;
    const char *d = slash + 1;
    int tneg = 0, dneg = 0;
    if (*t == 'M') {
        tneg = 1;
        t++;
    }
    size_t tlen = (size_t)(slash - t);
    if (tlen != 2 || !all_digit(t, 2))
        return 0;
    if (*d == 'M') {
        dneg = 1;
        d++;
    }
    size_t dlen = strlen(d);
    if (dlen != 2 || !all_digit(d, 2))
        return 0;
    int tval = (t[0] - '0') * 10 + (t[1] - '0');
    int dval = (d[0] - '0') * 10 + (d[1] - '0');
    out->temp_c = tneg ? -tval : tval;
    out->dewpoint_c = dneg ? -dval : dval;
    return 1;
}

int metar_parse_one(const char *text, MetarReport *out) {
    memset(out, 0, sizeof(*out));
    out->visibility_m = -1;
    out->qnh_hpa = -1;
    out->wind_gust = -1;
    out->wind_var_from = out->wind_var_to = -1;
    out->wind_dir = -1;

    const char *endp = strchr(text, '=');
    size_t len = endp ? (size_t)(endp - text) : strlen(text);

    char buf[512];
    if (len >= sizeof(buf))
        len = sizeof(buf) - 1;
    memcpy(buf, text, len);
    buf[len] = '\0';

    char *tok = strtok(buf, " \t\r\n");

    if (!tok || strlen(tok) != 4 || !all_upper_alpha(tok, 4))
        return 0;
    safe_copy(out->station, sizeof(out->station), tok);
    tok = strtok(NULL, " \t\r\n");

    if (!tok || strlen(tok) != 7 || !all_digit(tok, 6) || tok[6] != 'Z')
        return 0;
    out->day = (tok[0] - '0') * 10 + (tok[1] - '0');
    out->hour = (tok[2] - '0') * 10 + (tok[3] - '0');
    out->minute = (tok[4] - '0') * 10 + (tok[5] - '0');
    tok = strtok(NULL, " \t\r\n");

    if (tok && strcmp(tok, "AUTO") == 0)
        tok = strtok(NULL, " \t\r\n");

    if (tok && is_wind_group(tok, out)) {
        tok = strtok(NULL, " \t\r\n");
        if (tok && is_wind_variable_group(tok, out))
            tok = strtok(NULL, " \t\r\n");
    }

    if (tok && strcmp(tok, "CAVOK") == 0) {
        out->cavok = 1;
        tok = strtok(NULL, " \t\r\n");
    } else if (tok && strlen(tok) == 4 && all_digit(tok, 4)) {
        out->visibility_m = atoi(tok);
        tok = strtok(NULL, " \t\r\n");
    }

    while (tok && out->n_phenomena < 4 && is_phenomenon_token(tok)) {
        safe_copy(out->phenomena[out->n_phenomena].code, sizeof(out->phenomena[0].code), tok);
        out->n_phenomena++;
        tok = strtok(NULL, " \t\r\n");
    }

    while (tok && out->n_clouds < 4 && is_cloud_token(tok, &out->clouds[out->n_clouds])) {
        out->n_clouds++;
        tok = strtok(NULL, " \t\r\n");
    }

    if (tok && is_temp_dewpoint(tok, out)) {
        out->has_temp = 1;
        tok = strtok(NULL, " \t\r\n");
    }

    if (tok && tok[0] == 'Q' && strlen(tok) == 5 && all_digit(tok + 1, 4)) {
        out->qnh_hpa = atoi(tok + 1);
        tok = strtok(NULL, " \t\r\n");
    }

    if (tok) {
        size_t pos = 0;
        while (tok) {
            size_t tl = strlen(tok);
            if (pos + tl + 1 >= sizeof(out->trend))
                break;
            if (pos > 0)
                out->trend[pos++] = ' ';
            memcpy(out->trend + pos, tok, tl);
            pos += tl;
            tok = strtok(NULL, " \t\r\n");
        }
        out->trend[pos] = '\0';
    }

    out->valid = 1;
    return 1;
}

#define ANNOT(RAW, ...)                                                       \
    do {                                                                       \
        int _n1 = snprintf(line, sizeof(line), __VA_ARGS__);                   \
        (void)_n1;                                                             \
        int _n2 = snprintf(out + pos, out_size > pos ? out_size - pos : 0,     \
                            "  \"%s\"  ->  %s\n", (RAW), line);                \
        if (_n2 < 0 || pos + (size_t)_n2 >= out_size)                          \
            return;                                                            \
        pos += (size_t)_n2;                                                    \
    } while (0)

/* Разбирает сводку заново (та же логика, что metar_parse_one), но
 * вместо заполнения структуры построчно объясняет, что означает
 * КАЖДАЯ отдельная группа исходного текста - для показа пользователю
 * связи "кусок текста -> расшифровка". */
void metar_format_annotated(const char *text, char *out, size_t out_size) {
    size_t pos = 0;
    if (out_size)
        out[0] = '\0';
    char line[256];

    const char *endp = strchr(text, '=');
    size_t len = endp ? (size_t)(endp - text) : strlen(text);
    char buf[512];
    if (len >= sizeof(buf))
        len = sizeof(buf) - 1;
    memcpy(buf, text, len);
    buf[len] = '\0';

    MetarReport dummy;
    memset(&dummy, 0, sizeof(dummy));

    char *tok = strtok(buf, " \t\r\n");
    if (!tok || strlen(tok) != 4 || !all_upper_alpha(tok, 4))
        return;
    AirportInfo ap;
    if (airport_db_lookup(tok, &ap))
        ANNOT(tok, "аэродром: %s (%s, %s)", tok, ap.name, ap.municipality);
    else
        ANNOT(tok, "код аэродрома ICAO: %s", tok);
    tok = strtok(NULL, " \t\r\n");

    if (!tok || strlen(tok) != 7 || !all_digit(tok, 6) || tok[6] != 'Z')
        return;
    int day = (tok[0] - '0') * 10 + (tok[1] - '0');
    int hour = (tok[2] - '0') * 10 + (tok[3] - '0');
    int minute = (tok[4] - '0') * 10 + (tok[5] - '0');
    ANNOT(tok, "время сводки: %02d числа, %02d:%02d UTC", day, hour, minute);
    tok = strtok(NULL, " \t\r\n");

    if (tok && strcmp(tok, "AUTO") == 0) {
        ANNOT(tok, "автоматическая станция (без участия наблюдателя)");
        tok = strtok(NULL, " \t\r\n");
    }

    if (tok && is_wind_group(tok, &dummy)) {
        if (dummy.wind_variable) {
            if (dummy.wind_gust >= 0)
                ANNOT(tok, "ветер: переменное направление, %d %s, порывы до %d",
                      dummy.wind_speed, dummy.wind_unit, dummy.wind_gust);
            else
                ANNOT(tok, "ветер: переменное направление, %d %s", dummy.wind_speed,
                      dummy.wind_unit);
        } else {
            if (dummy.wind_gust >= 0)
                ANNOT(tok, "ветер: направление %d\302\260, скорость %d %s, порывы до %d",
                      dummy.wind_dir, dummy.wind_speed, dummy.wind_unit, dummy.wind_gust);
            else
                ANNOT(tok, "ветер: направление %d\302\260, скорость %d %s", dummy.wind_dir,
                      dummy.wind_speed, dummy.wind_unit);
        }
        tok = strtok(NULL, " \t\r\n");
        if (tok && is_wind_variable_group(tok, &dummy)) {
            ANNOT(tok, "направление ветра колеблется %d\302\260\342\200\223%d\302\260",
                  dummy.wind_var_from, dummy.wind_var_to);
            tok = strtok(NULL, " \t\r\n");
        }
    }

    if (tok && strcmp(tok, "CAVOK") == 0) {
        ANNOT(tok, "видимость без ограничений, облаков ниже 1500 м нет, опасных явлений нет");
        tok = strtok(NULL, " \t\r\n");
    } else if (tok && strlen(tok) == 4 && all_digit(tok, 4)) {
        int vis = atoi(tok);
        if (vis >= 9999)
            ANNOT(tok, "видимость: 10 км и более");
        else
            ANNOT(tok, "видимость: %d м", vis);
        tok = strtok(NULL, " \t\r\n");
    }

    while (tok && is_phenomenon_token(tok)) {
        char desc[64];
        metar_phenomenon_describe(tok, desc, sizeof(desc));
        ANNOT(tok, "явление погоды: %s", desc);
        tok = strtok(NULL, " \t\r\n");
    }

    MetarCloud cl;
    while (tok && is_cloud_token(tok, &cl)) {
        if (cl.height_ft < 0)
            ANNOT(tok, "облачность: %s",
                  strcmp(cl.type, "SKC") == 0   ? "ясно"
                  : strcmp(cl.type, "NCD") == 0 ? "облаков не обнаружено"
                                                 : "незначительная");
        else {
            const char *amount = strcmp(cl.type, "FEW") == 0   ? "незначительная"
                                 : strcmp(cl.type, "SCT") == 0 ? "разрозненная"
                                 : strcmp(cl.type, "BKN") == 0 ? "значительная"
                                                                : "сплошная";
            ANNOT(tok, "облачность: %s, высота нижней границы %d фт", amount, cl.height_ft);
        }
        tok = strtok(NULL, " \t\r\n");
    }

    if (tok && is_temp_dewpoint(tok, &dummy)) {
        ANNOT(tok, "температура: %d\302\260C, точка росы: %d\302\260C", dummy.temp_c,
              dummy.dewpoint_c);
        tok = strtok(NULL, " \t\r\n");
    }

    if (tok && tok[0] == 'Q' && strlen(tok) == 5 && all_digit(tok + 1, 4)) {
        ANNOT(tok, "давление (QNH): %d гПа", atoi(tok + 1));
        tok = strtok(NULL, " \t\r\n");
    }

    if (tok) {
        if (strcmp(tok, "NOSIG") == 0) {
            ANNOT(tok, "прогноз: существенных изменений не ожидается");
        } else {
            ANNOT(tok, "начало блока прогноза изменений (временных/устойчивых)");
        }
    }
}
#undef ANNOT

int metar_find_reports(const char *text, MetarReport *out, int max_reports) {
    int n = 0;
    const char *p = text;
    size_t tlen = strlen(text);

    for (size_t i = 0; i + 12 <= tlen && n < max_reports; i++) {
        const char *q = text + i;
        int at_start = (i == 0 || q[-1] == ' ' || q[-1] == '\n' || q[-1] == '\r' || q[-1] == '\t');
        if (!at_start)
            continue;
        if (!(isupper((unsigned char)q[0]) && isupper((unsigned char)q[1]) &&
              isupper((unsigned char)q[2]) && isupper((unsigned char)q[3])))
            continue;
        if (q[4] != ' ')
            continue;
        if (!all_digit(q + 5, 6))
            continue;
        if (q[11] != 'Z')
            continue;
        char after = q[12];
        if (after != ' ' && after != '\0' && after != '\r' && after != '\n')
            continue;

        if (metar_parse_one(q, &out[n])) {
            n++;
            const char *eq = strchr(q, '=');
            if (eq)
                i = (size_t)(eq - text); /* продолжаем поиск сразу после "=" этой сводки */
        }
    }
    (void)p;
    return n;
}

/* Перевод двухбуквенных кодов явлений погоды (ICAO Annex 3) на
 * русский - используется и в консольном, и в GUI-выводе. */
static const char *phenomenon_ru(const char *code2) {
    static const struct {
        const char *code;
        const char *ru;
    } table[] = {
        {"MI", "мелкий"}, {"BC", "клочьями"}, {"PR", "частично"}, {"DR", "низовой перенос"},
        {"BL", "метель"}, {"SH", "ливень"}, {"TS", "гроза"}, {"FZ", "переохлаждённый"},
        {"DZ", "морось"}, {"RA", "дождь"}, {"SN", "снег"}, {"SG", "снежная крупа"},
        {"IC", "ледяные иглы"}, {"PL", "ледяной дождь"}, {"GR", "град"}, {"GS", "мелкий град"},
        {"UP", "неизвестные осадки"},
        {"BR", "дымка"}, {"FG", "туман"}, {"FU", "дым"}, {"VA", "вулканический пепел"},
        {"DU", "пыль"}, {"SA", "песок"}, {"HZ", "мгла"},
        {"PO", "пыльный вихрь"}, {"SQ", "шквал"}, {"FC", "смерч"}, {"SS", "песчаная буря"},
        {"DS", "пыльная буря"}, {"VC", "поблизости"}, {"RE", "недавний"},
        {NULL, NULL}};
    for (int i = 0; table[i].code; i++)
        if (strcmp(code2, table[i].code) == 0)
            return table[i].ru;
    return code2;
}

void metar_phenomenon_describe(const char *tok, char *out, size_t out_size) {
    const char *intensity = "";
    const char *p = tok;
    if (*p == '+') {
        intensity = "сильн. ";
        p++;
    } else if (*p == '-') {
        intensity = "слаб. ";
        p++;
    }
    size_t pos = 0;
    int n = snprintf(out, out_size, "%s", intensity);
    pos = (n > 0 && (size_t)n < out_size) ? (size_t)n : out_size - 1;
    size_t len = strlen(p);
    for (size_t i = 0; i + 1 < len + 1 && i < len; i += 2) {
        char code[3] = {p[i], p[i + 1], '\0'};
        const char *ru = phenomenon_ru(code);
        n = snprintf(out + pos, out_size - pos, "%s%s", i > 0 ? ", " : "", ru);
        if (n < 0 || (size_t)n >= out_size - pos)
            break;
        pos += (size_t)n;
    }
}

void metar_format(const MetarReport *r, char *out, size_t out_size) {
    size_t pos = 0;
    int n;

#define APPEND(...)                                                        \
    do {                                                                    \
        n = snprintf(out + pos, out_size - pos, __VA_ARGS__);               \
        if (n < 0 || (size_t)n >= out_size - pos)                           \
            goto done;                                                      \
        pos += (size_t)n;                                                   \
    } while (0)

    AirportInfo ap;
    if (airport_db_lookup(r->station, &ap))
        APPEND("%s (%s, %s), %02d числа %02d:%02d UTC\n", r->station, ap.name, ap.municipality,
               r->day, r->hour, r->minute);
    else
        APPEND("%s, %02d числа %02d:%02d UTC\n", r->station, r->day, r->hour, r->minute);

    if (r->wind_variable)
        APPEND("  Ветер: переменный, %d %s", r->wind_speed, r->wind_unit);
    else
        APPEND("  Ветер: %d\302\260, %d %s", r->wind_dir, r->wind_speed, r->wind_unit);
    if (r->wind_gust >= 0)
        APPEND(", порывы до %d", r->wind_gust);
    if (r->wind_var_from >= 0)
        APPEND(" (колеблется %d\302\260\342\200\223%d\302\260)", r->wind_var_from, r->wind_var_to);
    APPEND("\n");

    if (r->cavok) {
        APPEND("  Видимость: без ограничений, облаков ниже 1500 м нет (CAVOK)\n");
    } else {
        if (r->visibility_m >= 9999)
            APPEND("  Видимость: 10 км и более\n");
        else if (r->visibility_m >= 0)
            APPEND("  Видимость: %d м\n", r->visibility_m);
        for (int i = 0; i < r->n_phenomena; i++) {
            char desc[64];
            metar_phenomenon_describe(r->phenomena[i].code, desc, sizeof(desc));
            APPEND("  Явление: %s\n", desc);
        }
        for (int i = 0; i < r->n_clouds; i++) {
            if (r->clouds[i].height_ft < 0) {
                APPEND("  Облачность: %s\n",
                       strcmp(r->clouds[i].type, "SKC") == 0   ? "ясно"
                       : strcmp(r->clouds[i].type, "NCD") == 0 ? "облаков не обнаружено"
                                                                : "незначительная");
            } else {
                const char *amount = strcmp(r->clouds[i].type, "FEW") == 0   ? "незначительная"
                                     : strcmp(r->clouds[i].type, "SCT") == 0 ? "разрозненная"
                                     : strcmp(r->clouds[i].type, "BKN") == 0 ? "значительная"
                                                                             : "сплошная";
                APPEND("  Облачность: %s, %d фт\n", amount, r->clouds[i].height_ft);
            }
        }
    }

    if (r->has_temp)
        APPEND("  Температура: %d\302\260C, точка росы %d\302\260C\n", r->temp_c, r->dewpoint_c);
    if (r->qnh_hpa >= 0)
        APPEND("  Давление (QNH): %d гПа\n", r->qnh_hpa);
    if (r->trend[0]) {
        if (strcmp(r->trend, "NOSIG") == 0)
            APPEND("  Прогноз: существенных изменений не ожидается\n");
        else
            APPEND("  Прогноз: %s\n", r->trend);
    }

done:
    out[out_size - 1] = '\0';
#undef APPEND
}
