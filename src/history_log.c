#include "history_log.h"
#include "fileio.h"
#include "labels.h"
#include "arinc622.h"
#include "aircraft_db.h"
#include "airport_db.h"
#include "cfdiu_fault.h"
#include "asu_telemetry.h"
#include "metar.h"
#include "ooi_parse.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define LOG_DIR "acars_logs"
#define CSV_PATH LOG_DIR "/acars_log.csv"
#define JSONL_PATH LOG_DIR "/acars_log.jsonl"
#define CSV_HEADER "timestamp,source,crc_status,mode,addr,acknak,label,label_description," \
                   "block_id,suffix,text,arinc622_imi,arinc622_imi_desc,arinc622_ground_station," \
                   "aircraft_manufacturer,aircraft_model,aircraft_typecode,aircraft_operator," \
                   "ooi_flight_id,ooi_departure,ooi_destination,ooi_arrival," \
                   "cfdiu_severity,cfdiu_description," \
                   "asu_fuel,asu_temp,asu_wind_dir,asu_wind_speed,asu_lat,asu_lon,asu_eta,asu_alt," \
                   "metar_count,metar_stations\n"
/* BOM (byte order mark) UTF-8 в начале CSV - без него Excel открывает
 * файл в системной кодовой странице (обычно CP1251) вместо UTF-8, и
 * русский текст превращается в "РћС‚С‡С‘С‚..." вместо "Отчёт...". С BOM
 * Excel правильно распознаёт кодировку сразу при открытии двойным
 * кликом. JSON Lines в BOM не нуждается (JSON-парсеры его не ждут). */
#define UTF8_BOM "\xEF\xBB\xBF"

#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION g_log_cs;
static void log_lock(void) { EnterCriticalSection(&g_log_cs); }
static void log_unlock(void) { LeaveCriticalSection(&g_log_cs); }
static void ensure_log_dir(void) { CreateDirectoryA(LOG_DIR, NULL); /* уже существует - и ладно */ }
void history_log_init(void) {
    InitializeCriticalSection(&g_log_cs);
    ensure_log_dir();
}
#else
#include <pthread.h>
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static void log_lock(void) { pthread_mutex_lock(&g_log_mutex); }
static void log_unlock(void) { pthread_mutex_unlock(&g_log_mutex); }
static void ensure_log_dir(void) { mkdir(LOG_DIR, 0755); /* уже существует - и ладно */ }
void history_log_init(void) { ensure_log_dir(); }
#endif

/* Путь к отдельному отчёту по текущему декодируемому WAV-файлу -
 * per-thread (не общий на весь процесс!): в GUI живой приём и
 * декодирование файла могут идти ПАРАЛЛЕЛЬНО в разных потоках, и
 * сообщения живого приёма не должны попадать в отчёт по файлу того
 * потока, который сейчас его декодирует, и наоборот. __thread - это
 * многолетнее, широко поддерживаемое расширение GCC/MinGW (не требует
 * специальной настройки C11 threads.h). */
static __thread char g_extra_path[512] = "";

/* Экранирование поля CSV по RFC 4180: оборачиваем в кавычки, каждую
 * внутреннюю кавычку удваиваем. src может быть NULL (тогда пишем
 * пустое поле). out должен быть в 2 раза больше src с запасом под кавычки. */
static void csv_escape(char *out, size_t out_size, const char *src) {
    if (!src) src = "";
    size_t o = 0;
    if (o < out_size - 1) out[o++] = '"';
    for (const char *p = src; *p && o < out_size - 2; p++) {
        if (*p == '"') {
            if (o < out_size - 2) out[o++] = '"';
        }
        out[o++] = *p;
    }
    if (o < out_size - 1) out[o++] = '"';
    out[o] = '\0';
}

/* Экранирование строки для JSON: кавычки, обратный слэш, управляющие
 * символы. out должен быть с запасом (минимум 2x от src + немного). */
static void json_escape(char *out, size_t out_size, const char *src) {
    if (!src) src = "";
    size_t o = 0;
    for (const char *p = src; *p && o < out_size - 2; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            if (o < out_size - 2) { out[o++] = '\\'; out[o++] = (char)c; }
        } else if (c == '\n') {
            if (o < out_size - 2) { out[o++] = '\\'; out[o++] = 'n'; }
        } else if (c == '\r') {
            if (o < out_size - 2) { out[o++] = '\\'; out[o++] = 'r'; }
        } else if (c == '\t') {
            if (o < out_size - 2) { out[o++] = '\\'; out[o++] = 't'; }
        } else if (c < 0x20) {
            if (o < out_size - 6) {
                sprintf(out + o, "\\u%04x", c);
                o += 6;
            }
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

/* Пишет одну CSV-строку сообщения в уже открытый файл (без заголовка -
 * заголовок пишется отдельно, один раз, тем кодом, который решает,
 * нужен ли он в этот момент). */
static void write_csv_row(FILE *f, const char *timestr, const char *source,
                           const AcarsMessage *m, const char *label_desc, const char *text) {
    char crc_status[16];
    if (m->crc_fixed)
        snprintf(crc_status, sizeof(crc_status), "FIXED%d", m->bits_fixed);
    else
        strcpy(crc_status, "OK");
    char mode_raw[2] = {m->mode ? m->mode : '?', '\0'};
    char ack_raw[2] = {m->acknak ? m->acknak : ' ', '\0'};
    char block_raw[2] = {m->block_id ? m->block_id : ' ', '\0'};
    const char *suffix = m->is_etb ? "ETB" : "ETX";

    Arinc622Envelope env;
    int has_env = arinc622_parse(text, m->addr, &env);

    AircraftInfo ac;
    int has_ac = m->addr[0] && aircraft_db_lookup(m->addr, &ac);

    OoiInfo ooi;
    int has_ooi = ooi_parse(text, &ooi);

    CfdiuFault fault;
    int has_fault = cfdiu_fault_parse(text, &fault);

    AsuTelemetry tel;
    int has_tel = asu_telemetry_parse(text, &tel);

    MetarReport metars[4];
    int n_metar = metar_find_reports(text, metars, 4);
    char metar_stations_raw[48] = "";
    for (int mi = 0; mi < n_metar; mi++) {
        if (mi > 0)
            strncat(metar_stations_raw, ",", sizeof(metar_stations_raw) - strlen(metar_stations_raw) - 1);
        strncat(metar_stations_raw, metars[mi].station,
                sizeof(metar_stations_raw) - strlen(metar_stations_raw) - 1);
    }
    char metar_count_str[8];
    snprintf(metar_count_str, sizeof(metar_count_str), "%d", n_metar);

    char e_source[256], e_addr[32], e_label[16], e_label_desc[256], e_text[600];
    char e_imi[16], e_imi_desc[256], e_gs[32];
    char e_mfr[128], e_model[128], e_typecode[32], e_operator[256];
    char e_flight[32], e_dep[16], e_dest[16], e_arr[16];
    char mode_str[8], ack_str[8], block_str[8];
    char e_fault_sev[16], e_fault_desc[256];
    char e_fuel[24], e_temp[24], e_wdir[24], e_wspd[24], e_lat[32], e_lon[32], e_eta[24], e_alt[24];
    char e_metar_stations[64];
    csv_escape(e_source, sizeof(e_source), source);
    csv_escape(e_addr, sizeof(e_addr), m->addr);
    csv_escape(e_label, sizeof(e_label), m->label);
    csv_escape(e_label_desc, sizeof(e_label_desc), label_desc);
    csv_escape(e_text, sizeof(e_text), text);
    csv_escape(e_imi, sizeof(e_imi), has_env ? env.imi : "");
    csv_escape(e_imi_desc, sizeof(e_imi_desc), has_env ? env.imi_description : "");
    csv_escape(e_gs, sizeof(e_gs), has_env ? env.ground_station : "");
    csv_escape(e_mfr, sizeof(e_mfr), has_ac ? ac.manufacturer : "");
    csv_escape(e_model, sizeof(e_model), has_ac ? ac.model : "");
    csv_escape(e_typecode, sizeof(e_typecode), has_ac ? ac.typecode : "");
    csv_escape(e_operator, sizeof(e_operator), has_ac ? ac.operator_name : "");
    csv_escape(e_flight, sizeof(e_flight), has_ooi ? ooi.flight_id : "");
    csv_escape(e_dep, sizeof(e_dep), has_ooi ? ooi.departure : "");
    csv_escape(e_dest, sizeof(e_dest), has_ooi ? ooi.destination : "");
    csv_escape(e_arr, sizeof(e_arr), has_ooi ? ooi.arrival : "");
    csv_escape(e_fault_sev, sizeof(e_fault_sev), has_fault ? fault.severity : "");
    csv_escape(e_fault_desc, sizeof(e_fault_desc), has_fault ? fault.description : "");
    csv_escape(e_fuel, sizeof(e_fuel), has_tel ? tel.fuel : "");
    csv_escape(e_temp, sizeof(e_temp), has_tel ? tel.temp : "");
    csv_escape(e_wdir, sizeof(e_wdir), has_tel ? tel.wind_dir : "");
    csv_escape(e_wspd, sizeof(e_wspd), has_tel ? tel.wind_speed : "");
    csv_escape(e_lat, sizeof(e_lat), has_tel ? tel.lat : "");
    csv_escape(e_lon, sizeof(e_lon), has_tel ? tel.lon : "");
    csv_escape(e_eta, sizeof(e_eta), has_tel ? tel.eta : "");
    csv_escape(e_alt, sizeof(e_alt), has_tel ? tel.alt : "");
    csv_escape(e_metar_stations, sizeof(e_metar_stations), metar_stations_raw);
    /* mode/acknak/block_id - однобайтовые поля кадра ACARS, но сам байт
     * иногда оказывается управляющим символом (например, NAK = 0x15
     * для отрицательного подтверждения - см. документацию по Ack/Nak) -
     * такой байт, вставленный в CSV/JSON "как есть", не экранированным,
     * либо портит вид ячейки, либо (в JSON - гораздо серьёзнее) делает
     * всю строку НЕВАЛИДНЫМ JSON, который многие читалки (в т.ч.
     * стандартный json-модуль Python) попросту откажутся разбирать.
     * Экранируем той же функцией, что и текстовые поля. */
    csv_escape(mode_str, sizeof(mode_str), mode_raw);
    csv_escape(ack_str, sizeof(ack_str), ack_raw);
    csv_escape(block_str, sizeof(block_str), block_raw);

    fprintf(f, "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
            timestr, e_source, crc_status, mode_str, e_addr, ack_str,
            e_label, e_label_desc, block_str, suffix, e_text, e_imi, e_imi_desc, e_gs,
            e_mfr, e_model, e_typecode, e_operator, e_flight, e_dep, e_dest, e_arr,
            e_fault_sev, e_fault_desc, e_fuel, e_temp, e_wdir, e_wspd, e_lat, e_lon, e_eta, e_alt,
            metar_count_str, e_metar_stations);
}

void history_log_start_file_report(const char *wav_path) {
    /* Кладём отчёт по имени файла (без пути) в папку acars_logs - даже
     * если сам WAV лежит совсем в другом месте, все результаты работы
     * программы теперь собраны в одном месте, а не разбросаны. */
    const char *slash = strrchr(wav_path, '/');
    const char *bslash = strrchr(wav_path, '\\');
    const char *base = slash;
    if (bslash && (!base || bslash > base))
        base = bslash;
    base = base ? base + 1 : wav_path;

    ensure_log_dir();
    snprintf(g_extra_path, sizeof(g_extra_path), LOG_DIR "/%s.decoded.csv", base);

    /* Создаём (перезаписываем, если декодируем этот же файл повторно)
     * СРАЗУ, с заголовком - даже если ни одного сообщения в итоге не
     * найдётся, файл всё равно появится: это подтверждение, что файл
     * был обработан, а не тишина без всякого результата. */
    FILE *f = fopen_utf8(g_extra_path, "w"); /* в имени WAV-файла может быть кириллица */
    if (f) {
        time_t now = time(NULL);
        struct tm *tm_now = localtime(&now);
        char timestr[32];
        strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", tm_now);
        fputs(UTF8_BOM, f);
        fprintf(f, "# Отчёт по декодированию: %s\n", wav_path);
        fprintf(f, "# Дата декодирования: %s\n", timestr);
        fprintf(f, CSV_HEADER);
        fclose(f);
    } else {
        g_extra_path[0] = '\0'; /* не удалось создать - не пытаемся дальше */
    }
}

void history_log_end_file_report(void) {
    g_extra_path[0] = '\0';
}

void history_log_message(const AcarsMessage *m, const char *source) {
    if (!m->crc_ok)
        return; /* сохраняем только достоверные сообщения, как и в окне вывода */

    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    char timestr[32];
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", tm_now);

    const char *label_desc = acars_label_lookup(m->label);
    const char *text = m->has_text ? m->text : "";

    log_lock();

    /* --- общий CSV (все сообщения из всех источников) --- */
    FILE *fcsv = fopen(CSV_PATH, "a+");
    if (fcsv) {
        fseek(fcsv, 0, SEEK_END);
        if (ftell(fcsv) == 0) {
            fputs(UTF8_BOM, fcsv);
            fprintf(fcsv, CSV_HEADER);
        }
        write_csv_row(fcsv, timestr, source, m, label_desc, text);
        fclose(fcsv);
    }

    /* --- отдельный отчёт по текущему декодируемому файлу (если открыт
     * в этом потоке через history_log_start_file_report) --- */
    if (g_extra_path[0]) {
        FILE *fextra = fopen_utf8(g_extra_path, "a");
        if (fextra) {
            write_csv_row(fextra, timestr, source, m, label_desc, text);
            fclose(fextra);
        }
    }

    /* --- JSON Lines (общий, как и раньше) --- */
    FILE *fjson = fopen(JSONL_PATH, "a");
    if (fjson) {
        char crc_status[16];
        if (m->crc_fixed)
            snprintf(crc_status, sizeof(crc_status), "FIXED%d", m->bits_fixed);
        else
            strcpy(crc_status, "OK");
        char mode_raw[2] = {m->mode ? m->mode : '?', '\0'};
        char ack_raw[2] = {m->acknak ? m->acknak : ' ', '\0'};
        char block_raw[2] = {m->block_id ? m->block_id : ' ', '\0'};
        const char *suffix = m->is_etb ? "ETB" : "ETX";

        char e_source[300], e_addr[40], e_label[20], e_label_desc[300], e_text[700];
        char e_imi[20], e_imi_desc[300], e_gs[40];
        char e_mfr[150], e_model[150], e_typecode[40], e_operator[300];
        char e_flight[40], e_dep[20], e_dest[20], e_arr[20];
        char mode_str[8], ack_str[8], block_str[8];
        char e_fault_sev[20], e_fault_desc[300];
        char e_fuel[30], e_temp[30], e_wdir[30], e_wspd[30], e_lat[40], e_lon[40], e_eta[30], e_alt[30];
        json_escape(e_source, sizeof(e_source), source);
        json_escape(e_addr, sizeof(e_addr), m->addr);
        json_escape(e_label, sizeof(e_label), m->label);
        json_escape(e_label_desc, sizeof(e_label_desc), label_desc);
        json_escape(e_text, sizeof(e_text), text);
        /* mode/acknak/block_id - см. подробный комментарий в
         * write_csv_row() выше: сырой управляющий байт (например, NAK
         * = 0x15) прямо внутри значения JSON-строки делает всю строку
         * НЕВАЛИДНЫМ JSON - многие читалки (например, стандартный
         * json-модуль Python) целиком откажутся её разбирать. Отсюда и
         * взялся реальный баг: почти 70% строк в одном из логов
         * пользователя не разбирались именно по этой причине. */
        json_escape(mode_str, sizeof(mode_str), mode_raw);
        json_escape(ack_str, sizeof(ack_str), ack_raw);
        json_escape(block_str, sizeof(block_str), block_raw);

        Arinc622Envelope env;
        int has_env = arinc622_parse(text, m->addr, &env);
        json_escape(e_imi, sizeof(e_imi), has_env ? env.imi : "");
        json_escape(e_imi_desc, sizeof(e_imi_desc), has_env ? env.imi_description : "");
        json_escape(e_gs, sizeof(e_gs), has_env ? env.ground_station : "");

        AircraftInfo ac;
        int has_ac = m->addr[0] && aircraft_db_lookup(m->addr, &ac);
        json_escape(e_mfr, sizeof(e_mfr), has_ac ? ac.manufacturer : "");
        json_escape(e_model, sizeof(e_model), has_ac ? ac.model : "");
        json_escape(e_typecode, sizeof(e_typecode), has_ac ? ac.typecode : "");
        json_escape(e_operator, sizeof(e_operator), has_ac ? ac.operator_name : "");

        OoiInfo ooi;
        int has_ooi = ooi_parse(text, &ooi);
        json_escape(e_flight, sizeof(e_flight), has_ooi ? ooi.flight_id : "");
        json_escape(e_dep, sizeof(e_dep), has_ooi ? ooi.departure : "");
        json_escape(e_dest, sizeof(e_dest), has_ooi ? ooi.destination : "");
        json_escape(e_arr, sizeof(e_arr), has_ooi ? ooi.arrival : "");

        CfdiuFault fault;
        int has_fault = cfdiu_fault_parse(text, &fault);
        json_escape(e_fault_sev, sizeof(e_fault_sev), has_fault ? fault.severity : "");
        json_escape(e_fault_desc, sizeof(e_fault_desc), has_fault ? fault.description : "");

        AsuTelemetry tel;
        int has_tel = asu_telemetry_parse(text, &tel);
        json_escape(e_fuel, sizeof(e_fuel), has_tel ? tel.fuel : "");
        json_escape(e_temp, sizeof(e_temp), has_tel ? tel.temp : "");
        json_escape(e_wdir, sizeof(e_wdir), has_tel ? tel.wind_dir : "");
        json_escape(e_wspd, sizeof(e_wspd), has_tel ? tel.wind_speed : "");
        json_escape(e_lat, sizeof(e_lat), has_tel ? tel.lat : "");
        json_escape(e_lon, sizeof(e_lon), has_tel ? tel.lon : "");
        json_escape(e_eta, sizeof(e_eta), has_tel ? tel.eta : "");
        json_escape(e_alt, sizeof(e_alt), has_tel ? tel.alt : "");

        MetarReport metars[4];
        int n_metar = metar_find_reports(text, metars, 4);
        char metar_stations_raw[48] = "";
        for (int mi = 0; mi < n_metar; mi++) {
            if (mi > 0)
                strncat(metar_stations_raw, ",",
                        sizeof(metar_stations_raw) - strlen(metar_stations_raw) - 1);
            strncat(metar_stations_raw, metars[mi].station,
                    sizeof(metar_stations_raw) - strlen(metar_stations_raw) - 1);
        }
        char metar_count_str[8];
        snprintf(metar_count_str, sizeof(metar_count_str), "%d", n_metar);
        char e_metar_stations[64];
        json_escape(e_metar_stations, sizeof(e_metar_stations), metar_stations_raw);

        fprintf(fjson,
                "{\"timestamp\":\"%s\",\"source\":\"%s\",\"crc_status\":\"%s\",\"mode\":\"%s\","
                "\"addr\":\"%s\",\"acknak\":\"%s\",\"label\":\"%s\",\"label_description\":\"%s\","
                "\"block_id\":\"%s\",\"suffix\":\"%s\",\"text\":\"%s\",\"arinc622_imi\":\"%s\","
                "\"arinc622_imi_desc\":\"%s\",\"arinc622_ground_station\":\"%s\","
                "\"aircraft_manufacturer\":\"%s\",\"aircraft_model\":\"%s\","
                "\"aircraft_typecode\":\"%s\",\"aircraft_operator\":\"%s\","
                "\"ooi_flight_id\":\"%s\",\"ooi_departure\":\"%s\","
                "\"ooi_destination\":\"%s\",\"ooi_arrival\":\"%s\","
                "\"cfdiu_severity\":\"%s\",\"cfdiu_description\":\"%s\","
                "\"asu_fuel\":\"%s\",\"asu_temp\":\"%s\",\"asu_wind_dir\":\"%s\",\"asu_wind_speed\":\"%s\","
                "\"asu_lat\":\"%s\",\"asu_lon\":\"%s\",\"asu_eta\":\"%s\",\"asu_alt\":\"%s\","
                "\"metar_count\":\"%s\",\"metar_stations\":\"%s\"}\n",
                timestr, e_source, crc_status, mode_str, e_addr, ack_str,
                e_label, e_label_desc, block_str, suffix, e_text, e_imi, e_imi_desc, e_gs,
                e_mfr, e_model, e_typecode, e_operator, e_flight, e_dep, e_dest, e_arr,
                e_fault_sev, e_fault_desc, e_fuel, e_temp, e_wdir, e_wspd, e_lat, e_lon, e_eta, e_alt,
                metar_count_str, e_metar_stations);
        fclose(fjson);
    }

    log_unlock();
}
