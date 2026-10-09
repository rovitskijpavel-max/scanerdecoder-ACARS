#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "wavio.h"
#include "demod.h"
#include "framesync.h"
#include "labels.h"
#include "ifdemod.h"
#include "squelch.h"
#include "bitsync.h"
#include "coherent_am.h"
#include "mskpll.h"
#include "app.h"
#include "history_log.h"
#include "msg_sink.h"
#include "arinc622.h"
#include "ooi_parse.h"
#include "aircraft_db.h"
#include "airport_db.h"
#include "cfdiu_fault.h"
#include "asu_telemetry.h"
#include "acars_report.h"
#include "units.h"
#include "metar.h"
#ifdef _WIN32
#include "audio_win.h"
#include "ar5000.h"
#endif

#define MAX_BURSTS 128
#define MAX_DEDUP_FRAMES 32

/* Печатает код аэропорта, дописывая рядом название/город, если код
 * нашёлся в airport_db.csv (не найдётся - просто покажет один код,
 * без ошибки: обогащение полностью необязательное). */
static void print_airport_field(const char *label, const char *icao) {
    AirportInfo ap;
    if (airport_db_lookup(icao, &ap))
        printf("%s%s  (%s, %s)\n", label, icao, ap.name, ap.municipality);
    else
        printf("%s%s\n", label, icao);
}

static void print_message(const AcarsMessage *m) {
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    char timestr[16];
    strftime(timestr, sizeof(timestr), "%H:%M:%S", tm_now);

    printf("---------------------------------------------\n");
    if (m->crc_fixed) {
        printf("[%s] CRC: OK (исправлено %d бит%s из %d, возможна ложная правка)\n",
               timestr, m->bits_fixed, m->bits_fixed == 1 ? "" : "а", m->bits_fixed);
    } else {
        printf("[%s] CRC: %s\n", timestr, m->crc_ok ? "OK" : "ОШИБКА (искажённое сообщение)");
    }
    printf("  Mode     : %c\n", m->mode ? m->mode : '?');
    printf("  Addr     : %s\n", m->addr[0] ? m->addr : "(?)");
    {
        AircraftInfo ac;
        if (m->addr[0] && aircraft_db_lookup(m->addr, &ac)) {
            printf("             -> %s %s", ac.manufacturer, ac.model);
            if (ac.typecode[0])
                printf(" (%s)", ac.typecode);
            if (ac.operator_name[0])
                printf(", %s", ac.operator_name);
            printf("\n");
        }
    }
    printf("  Ack/Nak  : %c\n", m->acknak ? m->acknak : '?');
    if (m->label[0]) {
        printf("  Label    : %s  (%s)\n", m->label, acars_label_lookup(m->label));
    }
    if (m->block_id)
        printf("  Block id : %c\n", m->block_id);
    printf("  Suffix   : %s\n", m->is_etb ? "ETB (продолжение следует)" : "ETX");
    if (m->has_text) {
        printf("  Text     :\n%s\n", m->text);

        {
            char rep[4096];
            if (acars_report_decode(m->label, m->text, rep, sizeof(rep))) {
                printf("  --- Расшифровка ---\n%s", rep);
            }
        }

        Arinc622Envelope env;
        if (arinc622_parse(m->text, m->addr, &env)) {
            printf("  --- Конверт ARINC 622 (ADS-C/CPDLC) ---\n");
            printf("  Наземная станция : %s\n", env.ground_station);
            printf("  Тип (IMI)        : %s  (%s)\n", env.imi, env.imi_description);
            printf("  Борт             : %s\n", env.aircraft_reg);
            printf("  Внутр. CRC       : %s\n", env.inner_crc);
            printf("  Данные (hex, %d байт): %s\n", env.hex_payload_len / 2, env.hex_payload);
            printf("  (двоичные данные ASN.1 не расшифровываются - нужен специализированный\n"
                   "   декодер под схемы FANS-1/A CPDLC/ADS-C, например библиотека libacars;\n"
                   "   здесь надёжно выделяется только конверт - кто и о чём сообщение)\n");
        }

        OoiInfo ooi;
        if (ooi_parse(m->text, &ooi)) {
            printf("  --- Маршрут (формат OOOI FI, см. README) ---\n");
            if (ooi.flight_id[0])
                printf("  Рейс             : %s\n", ooi.flight_id);
            if (ooi.departure[0])
                print_airport_field("  Вылет из         : ", ooi.departure);
            if (ooi.destination[0])
                print_airport_field("  Назначение       : ", ooi.destination);
            if (ooi.arrival[0])
                print_airport_field("  Аэропорт (ARR)   : ", ooi.arrival);
            if (ooi.out_time[0])
                printf("  Время OUT (UTC)  : %s\n", ooi.out_time);
            if (ooi.off_time[0])
                printf("  Время OFF (UTC)  : %s\n", ooi.off_time);
            if (ooi.on_time[0])
                printf("  Время ON (UTC)   : %s\n", ooi.on_time);
            if (ooi.in_time[0])
                printf("  Время IN (UTC)   : %s\n", ooi.in_time);
            printf("  (аэропорты обычно кодом ICAO; это ОДИН из множества форматов -\n"
                   "   многие авиакомпании используют другой текст для той же информации,\n"
                   "   и тогда этот разбор просто не сработает - см. README)\n");
        } else {
            /* Формат "FI ..." не нашёлся - но в тексте иногда всё равно
             * встречаются ДВА идущих подряд, без разделителя, кода ICAO
             * (см. airport_db_scan_pair в airport_db.c) - такое
             * наблюдалось, например, в фирменной телеметрии "#DFBA"
             * (метка H1). Мы НЕ знаем, какое из двух - вылет, а какое -
             * назначение (это уже вопрос конкретного формата, которого
             * у нас нет), поэтому не подписываем роль, только сам факт
             * совпадения кодов - честно, без домыслов. */
            AirportInfo ap1, ap2;
            if (m->has_text && airport_db_scan_pair(m->text, &ap1, &ap2)) {
                printf("  --- В тексте замечены подряд идущие коды аэропортов ---\n");
                printf("  %s - %s, %s\n", ap1.icao, ap1.name, ap1.municipality);
                printf("  %s - %s, %s\n", ap2.icao, ap2.name, ap2.municipality);
                printf("  (порядок вылет/назначение неизвестен - формат этого конкретного\n"
                       "   сообщения не задокументирован, см. README)\n");
            }
        }

        CfdiuFault fault;
        if (cfdiu_fault_parse(m->text, &fault)) {
            printf("  --- Отчёт CFDIU (%s) ---\n",
                   strcmp(fault.severity, "WRN") == 0 ? "предупреждение" : "отказ");
            printf("  %s\n", fault.description);
            printf("  (код: %s/%s %s - точная расшифровка кода не подтверждена, см. README)\n",
                   fault.severity, fault.code2, fault.timestamp_code);
        }

        AsuTelemetry tel;
        if (asu_telemetry_parse(m->text, &tel)) {
            printf("  --- Телеметрия (формат ASU/AFL, см. README) ---\n");
            if (tel.fuel[0])
                printf("  Топливо          : %s\n", tel.fuel);
            if (tel.temp[0])
                printf("  Температура      : %s\n", tel.temp);
            if (tel.wind_dir[0] || tel.wind_speed[0])
                printf("  Ветер            : напр. %s, скорость %s\n", tel.wind_dir, tel.wind_speed);
            if (tel.lat[0] || tel.lon[0])
                printf("  Координаты       : %s, %s\n", tel.lat, tel.lon);
            if (tel.eta[0])
                printf("  ETA              : %s\n", tel.eta);
            if (tel.alt[0])
                { char at[48]; alt_ft_text(tel.alt, at, sizeof(at), NULL);
                printf("  Высота           : %s\n", at); }
        }

        MetarReport metars[4];
        int n_metar = metar_find_reports(m->text, metars, 4);
        if (n_metar > 0) {
            printf("  --- Погода (METAR, ICAO Annex 3) ---\n");
            for (int mi = 0; mi < n_metar; mi++) {
                char mbuf[1024];
                metar_format(&metars[mi], mbuf, sizeof(mbuf));
                printf("%s", mbuf);
            }
        }
    }
    printf("---------------------------------------------\n\n");
}

/* --- Основной метод: согласованный фильтр + Гарднер-ФАПЧ (mskpll.c) ---
 * Честный порт настоящего демодулятора acarsdec. Не нуждается ни в
 * сквелче, ни в переборе фазы - сам отслеживает дрейф тактовой частоты
 * передатчика на протяжении всего канала через ФАПЧ. На реальной
 * эталонной записи (test.wav проекта acarsdec) именно этот метод
 * достаёт все 7 сообщений; старый метод (простой дискриминатор +
 * перебор фазы, ниже) на ней не находил ни одного. */

typedef struct {
    FrameSync fs;
    const char *channel_label;
    int count;
} PllDecodeCtx;

static void on_bit_pll(int bit, void *ctx) {
    PllDecodeCtx *pc = (PllDecodeCtx *)ctx;
    framesync_feed_bit(&pc->fs, bit);
}

static void on_msg_pll(const AcarsMessage *m, void *ctx) {
    PllDecodeCtx *pc = (PllDecodeCtx *)ctx;
    if (!m->crc_ok)
        return;
    pc->count++;
    print_message(m);
    history_log_message(m, pc->channel_label);
    msg_sink_notify(m, pc->channel_label);
}

/* baseband_48k - уже готовый ACARS-звук (тоны 1200/2400 Гц) на 48 кГц
 * (для "сырого" IF - уже прогнанный через АМ-детектор огибающей).
 * mskpll.c работает на "родной" для алгоритма частоте 12500 Гц - в
 * эту частоту и передискретизируем перед подачей в демодулятор. */
static int decode_with_pll(const float *baseband_48k, size_t len_48k,
                            const char *channel_label, int verbose) {
    float *if_audio = NULL;
    size_t if_count = 0;
    if (resample_linear(baseband_48k, len_48k, MSK_TARGET_FS, MSKPLL_IF_RATE_HZ,
                         &if_audio, &if_count) != 0)
        return 0;

    PllDecodeCtx pc;
    pc.channel_label = channel_label;
    pc.count = 0;
    framesync_init(&pc.fs, on_msg_pll, &pc);

    MskPll pll;
    mskpll_init(&pll, on_bit_pll, &pc);
    mskpll_process(&pll, if_audio, (int)if_count);

    free(if_audio);

    if (verbose)
        printf("%s  Согласованный фильтр + ФАПЧ: найдено %d сообщений\n",
               channel_label, pc.count);
    return pc.count;
}

/* --- Резервный метод (если основной ничего не нашёл): старый простой
 * дискриминатор (demod.c) с перебором фазы битовой синхронизации по
 * каждому всплеску отдельно (squelch.c + bitsync.c). Полезен как
 * страховка, но на реальных сигналах уступает mskpll.c - см. README. --- */

typedef struct {
    uint8_t raw[ACARS_MAX_PAYLOAD];
    int raw_len;
} SeenFrame;

/* Общий контекст декодирования одного всплеска на одной конкретной
 * фазе битовой синхронизации. */
typedef struct {
    FrameSync fs;
    SeenFrame *seen;
    int *seen_count;
    int max_seen;
    int phase;
    double burst_start_sec;
    int found_any;   /* нашли ли на этой фазе хоть один валидный (crc_ok) кадр */
    int verbose;
    const char *channel_label;
} BurstDecodeCtx;

static void on_bit_burst(int bit, void *ctx) {
    BurstDecodeCtx *bc = (BurstDecodeCtx *)ctx;
    framesync_feed_bit(&bc->fs, bit);
}

static void on_msg_burst(const AcarsMessage *m, void *ctx) {
    BurstDecodeCtx *bc = (BurstDecodeCtx *)ctx;

    if (!m->crc_ok) {
        if (bc->verbose)
            printf("  (фаза %2d: кадр найден, CRC не сошёлся - отброшен)\n", bc->phase);
        return; /* нас интересуют только достоверные (в т.ч. исправленные) кадры */
    }

    for (int i = 0; i < *bc->seen_count; i++) {
        if (bc->seen[i].raw_len == m->raw_len &&
            memcmp(bc->seen[i].raw, m->raw, (size_t)m->raw_len) == 0)
            return; /* такой же кадр уже показывали (другая фаза этого же всплеска) */
    }
    if (*bc->seen_count < bc->max_seen) {
        memcpy(bc->seen[*bc->seen_count].raw, m->raw, (size_t)m->raw_len);
        bc->seen[*bc->seen_count].raw_len = m->raw_len;
        (*bc->seen_count)++;
    }

    bc->found_any = 1;
    printf("(всплеск на %.2f с, фаза синхронизации %d отсчётов)\n",
           bc->burst_start_sec, bc->phase);
    print_message(m);
    history_log_message(m, bc->channel_label);
    msg_sink_notify(m, bc->channel_label);
}

/* Прогоняет один всплеск сигнала через демодулятор+framesync на заданной
 * фазе. Возвращает 1, если найден хотя бы один новый достоверный кадр. */
static int try_phase(const float *seg, size_t len, int phase, double burst_start_sec,
                      SeenFrame *seen, int *seen_count, int max_seen, int verbose,
                      const char *channel_label) {
    if ((size_t)phase >= len)
        return 0;

    BurstDecodeCtx bc;
    bc.seen = seen;
    bc.seen_count = seen_count;
    bc.max_seen = max_seen;
    bc.phase = phase;
    bc.burst_start_sec = burst_start_sec;
    bc.found_any = 0;
    bc.verbose = verbose;
    bc.channel_label = channel_label;
    framesync_init(&bc.fs, on_msg_burst, &bc);

    MskDemod demod;
    msk_init(&demod, on_bit_burst, &bc);
    msk_process(&demod, seg + phase, (int)(len - (size_t)phase));

    return bc.found_any;
}

/* Декодирует один всплеск сигнала: сперва пробует фазу, предложенную
 * Гёрцель-оценкой (bitsync.c) - это быстро и в большинстве случаев
 * верно; если она не дала ни одного достоверного кадра, перебирает все
 * остальные MSK_SAMPLES_PER_BIT-1 фаз как страховку. Возвращает число
 * новых уникальных достоверных сообщений. */
static int decode_burst(const float *seg, size_t len, double burst_start_sec, int verbose,
                         const char *channel_label) {
    SeenFrame seen[MAX_DEDUP_FRAMES];
    int seen_count = 0;

    int phase0 = bitsync_best_phase(seg, len, MSK_TARGET_FS, MSK_SAMPLES_PER_BIT, 2400.0);
    if (verbose)
        printf("  Всплеск %.2f с (%.0f мс): оценка Гёрцеля предлагает фазу %d\n",
               burst_start_sec, 1000.0 * (double)len / MSK_TARGET_FS, phase0);

    int ok = try_phase(seg, len, phase0, burst_start_sec, seen, &seen_count, MAX_DEDUP_FRAMES,
                        verbose, channel_label);

    if (!ok) {
        if (verbose)
            printf("  Всплеск %.2f с: фаза Гёрцеля не дала верного CRC - перебираю остальные %d фаз\n",
                   burst_start_sec, MSK_SAMPLES_PER_BIT - 1);
        for (int off = 0; off < MSK_SAMPLES_PER_BIT; off++) {
            if (off == phase0)
                continue;
            try_phase(seg, len, off, burst_start_sec, seen, &seen_count, MAX_DEDUP_FRAMES,
                      verbose, channel_label);
        }
    }

    return seen_count;
}

static int decode_channel_fallback(const float *resampled, size_t rcount, int if_detected,
                                    const char *channel_label, int verbose) {
    /* Делим сигнал на всплески энергетическим сквелчем по сырым
     * отсчётам (до какой-либо демодуляции - амплитуда всё равно растёт
     * во время любой передачи, что для IF, что для готового звука).
     * Декодируем (несущая, фаза) отдельно внутри каждого всплеска, а
     * не по всему файлу разом, чтобы шум между передачами не портил
     * ни оценку несущей/фазы, ни не плодил кучу ложных кадров. */
    SquelchBurst bursts[MAX_BURSTS];
    int nb = squelch_find_bursts(resampled, rcount, MSK_TARGET_FS,
                                  /*threshold_factor=*/4.0,
                                  /*min_burst_ms=*/60.0,
                                  /*min_gap_ms=*/120.0,
                                  bursts, MAX_BURSTS);
    int nb_used = nb;
    if (nb_used > MAX_BURSTS)
        nb_used = MAX_BURSTS;

    if (verbose)
        printf("%s  Резервный конвейер, сквелч: найдено всплесков сигнала: %d\n", channel_label, nb);

    if (nb_used == 0) {
        bursts[0].start = 0;
        bursts[0].end = rcount;
        nb_used = 1;
    }

    int total = 0;
    for (int i = 0; i < nb_used; i++) {
        size_t start = bursts[i].start;
        size_t end = bursts[i].end;
        if (end <= start)
            continue;
        size_t len = end - start;
        double t0 = (double)start / MSK_TARGET_FS;

        /* Копируем отсчёты всплеска в отдельный буфер - демодуляция
         * (диодная или когерентная) меняет их на месте, а исходный
         * resampled нам ещё нужен для остальных всплесков. */
        float *seg = (float *)malloc(len * sizeof(float));
        if (!seg)
            continue;
        memcpy(seg, resampled + start, len * sizeof(float));

        if (if_detected) {
            /* Диодный (некогерентный) детектор быстрый и не требует
             * знания частоты несущей - пробуем его первым. Полосовой
             * фильтр когерентного метода даёт заметную выгоду на
             * чистом сигнале, но вносит фазовые искажения тонов
             * 1200/2400 Гц, которые на зашумлённом сигнале иногда
             * могут навредить больше, чем помогает избирательность по
             * несущей - поэтому не заменяем диодный метод когерентным,
             * а пробуем когерентный ВТОРЫМ заходом, только если первый
             * не дал ни одного достоверного кадра. */
            float *seg_diode = (float *)malloc(len * sizeof(float));
            int found_diode = 0;
            if (seg_diode) {
                memcpy(seg_diode, seg, len * sizeof(float));
                am_envelope_detect_inplace(seg_diode, len, MSK_TARGET_FS);
                found_diode = decode_burst(seg_diode, len, t0, verbose, channel_label);
                free(seg_diode);
            }

            if (found_diode > 0) {
                if (verbose)
                    printf("%s  Всплеск #%d: %.2f - %.2f с (%zu отсчётов), диодный детектор справился\n",
                           channel_label, i + 1, t0, (double)end / MSK_TARGET_FS, len);
                total += found_diode;
                free(seg);
                continue;
            }

            double conf = 0.0;
            double carrier = coherent_am_estimate_carrier_hz(
                seg, len, MSK_TARGET_FS,
                /*f_lo=*/1500.0, /*f_hi=*/(double)MSK_TARGET_FS / 2.0 - 1500.0,
                /*coarse_step_hz=*/50.0, &conf);

            const double MIN_CONFIDENCE = 3.0;
            if (conf < MIN_CONFIDENCE) {
                /* Несущая не определяется уверенно - когерентный метод
                 * без надёжной частоты бесполезен (и его не запускаем),
                 * а диодный уже испробован выше и ничего не дал. */
                if (verbose)
                    printf("%s  Всплеск #%d: диодный детектор ничего не дал, несущая для когерентного"
                           " не определяется уверенно (%.1fx < %.1fx) - пропускаю\n",
                           channel_label, i + 1, conf, MIN_CONFIDENCE);
                free(seg);
                continue;
            }

            if (verbose)
                printf("%s  Всплеск #%d: диодный детектор ничего не дал, пробую когерентный"
                       " (несущая ~%.0f Гц, уверенность %.1fx)\n",
                       channel_label, i + 1, carrier, conf);
            coherent_am_envelope_inplace(seg, len, MSK_TARGET_FS, carrier, /*bandwidth_hz=*/8000.0);
        }

        if (verbose)
            printf("%s  Всплеск #%d: %.2f - %.2f с (%zu отсчётов)\n", channel_label, i + 1, t0,
                   (double)end / MSK_TARGET_FS, len);

        total += decode_burst(seg, len, t0, verbose, channel_label);
        free(seg);
    }

    return total;
}

static int decode_channel(const float *samples, size_t count, int rate,
                           const char *channel_label, int if_mode, int verbose) {
    float *resampled = NULL;
    size_t rcount = 0;
    if (resample_linear(samples, count, rate, MSK_TARGET_FS, &resampled, &rcount) != 0)
        return 0;

    /* Некоторые записи (например, "сырой" IF-дамп со сканера/SDR без
     * встроенного АМ-детектора) содержат несущую в несколько/десятков
     * кГц, а не уже готовый звук с тонами 1200/2400 Гц. Определяем это
     * по средней частоте переходов через ноль (--if / --audio позволяют
     * задать режим вручную). */
    double zcr_hz = 0.0;
    int if_detected = ifdemod_decide_mode(resampled, rcount, MSK_TARGET_FS, if_mode, &zcr_hz);
    printf("%s  Средняя частота переходов через ноль: %.0f Гц -> %s\n", channel_label, zcr_hz,
           if_detected ? "похоже на сырой IF, потребуется АМ-демодуляция"
                       : "похоже на готовый ACARS-звук, АМ-демодуляция не нужна");

    /* Основной метод: mskpll.c (согласованный фильтр + Гарднер-ФАПЧ).
     * Для "сырого" IF сначала нужен АМ-детектор огибающей - берём
     * диодный (самый простой, не завязан на оценку несущей) и гоним
     * его по всему каналу разом (сквелч тут не нужен - PLL сам
     * прекрасно переживает паузы/тишину между передачами). */
    float *baseband = (float *)malloc(rcount * sizeof(float));
    int total = 0;
    if (baseband) {
        memcpy(baseband, resampled, rcount * sizeof(float));
        if (if_detected)
            am_envelope_detect_inplace(baseband, rcount, MSK_TARGET_FS);
        total = decode_with_pll(baseband, rcount, channel_label, verbose);
        free(baseband);
    }

    if (total > 0) {
        free(resampled);
        return total;
    }

    /* Основной метод не нашёл ни одного сообщения - подстраховываемся
     * старым конвейером (сквелч по всплескам + диодный/когерентный
     * АМ-демод + перебор фазы битовой синхронизации). На реальных
     * сигналах он слабее mskpll.c, но лишним не будет. */
    if (verbose)
        printf("%s  Основной метод (ФАПЧ) ничего не нашёл - пробую резервный конвейер\n", channel_label);

    total += decode_channel_fallback(resampled, rcount, if_detected, channel_label, verbose);

    free(resampled);
    return total;
}

static const char *path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *last = slash;
    if (bslash && (!last || bslash > last))
        last = bslash;
    return last ? last + 1 : path;
}

int decode_wav_file(const char *path, int if_mode, int verbose) {
    /* Отдельный отчёт по этому конкретному файлу (в дополнение к общему
     * acars_log.csv/.jsonl) - создаётся сразу, ДО декодирования, чтобы
     * появиться, даже если декодирование ничего не найдёт или вообще
     * не сможет открыть файл. Обязательно закрыть на каждом выходе из
     * функции (history_log_end_file_report), иначе поток "застрянет"
     * с чужим отчётом открытым при следующем вызове decode_wav_file. */
    history_log_start_file_report(path);

    int channels = 1;
    if (wav_get_num_channels(path, &channels) != 0) {
        history_log_end_file_report();
        return -1;
    }

    if (channels <= 1) {
        float *samples = NULL;
        size_t count = 0;
        int rate = 0;
        if (wav_load_mono(path, &samples, &count, &rate) != 0) {
            history_log_end_file_report();
            return -1;
        }

        printf("Файл: %s  (%d Гц, %.2f сек)\n", path, rate, (double)count / rate);
        int total = decode_channel(samples, count, rate, path_basename(path), if_mode, verbose);
        free(samples);

        printf("Готово. Найдено достоверных сообщений: %d\n\n", total);
        history_log_end_file_report();
        return 0;
    }

    /* Многоканальный файл (например, штатный test.wav проекта acarsdec -
     * 4 независимых радиоканала в одном WAV) - усреднять каналы в моно
     * НЕЛЬЗЯ, это смешает разные, не связанные друг с другом сообщения
     * в кашу. Декодируем каждый канал отдельно, как независимый поток. */
    printf("Файл: %s (%d каналов - декодирую каждый отдельно)\n", path, channels);

    int total = 0;
    for (int c = 0; c < channels; c++) {
        float *samples = NULL;
        size_t count = 0;
        int rate = 0;
        if (wav_load_channel(path, c, &samples, &count, &rate) != 0)
            continue;

        char label[300];
        snprintf(label, sizeof(label), "%s [канал %d/%d]", path_basename(path), c + 1, channels);
        printf("%s %d Гц, %.2f сек\n", label, rate, (double)count / rate);

        total += decode_channel(samples, count, rate, label, if_mode, verbose);
        free(samples);
    }

    printf("Готово. Найдено достоверных сообщений (все каналы): %d\n\n", total);
    history_log_end_file_report();
    return 0;
}

static void print_usage(const char *argv0) {
    printf("ACARS-декодер (аудиовход, для приёмников без IQ-выхода вроде AOR AR5000)\n\n");
    printf("Использование:\n");
    printf("  %s -f файл.wav [-f файл2.wav ...]   декодировать из WAV-файлов\n", argv0);
    printf("  %s --if -f файл.wav                 принудительно считать файл \"сырым IF\"\n", argv0);
    printf("                                        (несущая на кГц, не продетектированный АМ звук)\n");
    printf("                                        и применить АМ-детектор огибающей\n");
    printf("  %s --audio -f файл.wav              принудительно считать файл уже готовым\n", argv0);
    printf("  %s --no-if -f файл.wav              ACARS-звуком, без АМ-детектора (синоним --audio)\n", argv0);
    printf("                                        (по умолчанию решение принимается автоматически)\n");
    printf("  %s -v                                подробный вывод (всплески, фазы, отброшенные кадры)\n", argv0);
#ifdef _WIN32
    printf("  %s -l [номер_устройства]            живой приём со звуковой карты (Win32 waveIn, работает от XP)\n", argv0);
    printf("  %s --list-devices                   показать список устройств записи\n", argv0);
    printf("  %s --gui                            открыть графический интерфейс явно\n", argv0);
    printf("  (запуск двойным кликом, без параметров, тоже открывает графический интерфейс)\n");
    printf("  %s --ar5000-port COM3 --ar5000-freq 131.725 -l\n", argv0);
    printf("                                        подключить AR5000 по RS232 перед живым приёмом:\n");
    printf("                                        настроить режим AM, полосу 6 кГц и указанную частоту.\n");
    printf("                                        --ar5000-baud 4800|9600|19200 (по умолчанию 9600) -\n");
    printf("                                        должна совпадать с CONFIG-меню самого приёмника.\n");
#else
    printf("  (живой приём собирается только под Windows, см. audio_win.c)\n");
#endif
}

#ifdef _WIN32
int run_gui(void);
#endif

int main(int argc, char **argv) {
    history_log_init(); /* готовим блокировку для потокобезопасной записи в лог */
#ifdef _WIN32
    ar5000_module_init(); /* готовим блокировку для потокобезопасного обмена по COM-порту */
#endif
    aircraft_db_load(); /* необязательно: aircraft_db.csv (формат OpenSky Network) рядом
                          * с программой, если есть - обогащает Addr данными о самолёте.
                          * Само сообщение об успешной загрузке печатаем НИЖЕ, отдельно
                          * для консольного и GUI-режимов (см. комментарий там) - если
                          * напечатать его прямо здесь, в GUI-режиме оно уйдёт в
                          * мелькающее окно консоли ДО того, как run_gui() успеет
                          * перенаправить stdout в окно вывода, и просто потеряется. */
    airport_db_load(); /* необязательно: airport_db.csv (формат OurAirports) рядом с
                         * программой, если есть - расшифровывает коды ICAO аэропортов
                         * (например UUEE -> Sheremetyevo, Moscow) в маршрутах. */

#ifdef _WIN32
    /* По умолчанию cmd.exe использует старую кодовую страницу (обычно
     * CP866/CP1251), а исходники программы в UTF-8 - без этого русский
     * текст выводится кракозябрами. SetConsoleOutputCP(CP_UTF8) сама по
     * себе появилась в Windows 2000, но реальную UTF-8 отрисовку консоль
     * получила только в районе Windows 10 (1903) - на XP/Vista/7/8 этот
     * вызов молча завершится неудачей, и текст всё равно будет
     * нечитаемым. Рабочий обход для старых систем - перенаправить вывод
     * в файл (acarsdecoder.exe -l 2 > log.txt) и открыть его в любом
     * редакторе, понимающем UTF-8 (Notepad++ и т.п.): сами байты в файле
     * будут корректным UTF-8 независимо от кодовой страницы консоли.
     * В графическом режиме (run_gui) этой проблемы нет вообще - там
     * текст выводится в обычное окно Windows, а не в консоль. */
    if (!SetConsoleOutputCP(CP_UTF8) || !SetConsoleCP(CP_UTF8)) {
        fprintf(stderr,
                "Внимание: эта версия Windows не поддерживает вывод UTF-8 в консоль напрямую -\n"
                "русский текст ниже может отображаться нечитаемо. Обход: перенаправьте вывод в\n"
                "файл (%s ... > log.txt) и откройте файл в редакторе с поддержкой UTF-8.\n\n",
                argv[0]);
    }

    /* Запуск без параметров (в том числе просто двойной клик по .exe в
     * Проводнике) теперь открывает графический интерфейс, а не сразу
     * закрывающееся окно консоли со справкой. Явный --gui делает то же
     * самое, даже если запущено из консоли. */
    if (argc < 2 || (argc == 2 && strcmp(argv[1], "--gui") == 0))
        return run_gui();
#endif

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    /* Консольный режим (был GUI - или его нет на этой платформе, или
     * пользователь явно запросил параметры командной строки) - здесь
     * stdout всегда идёт прямо в реальную консоль, можно печатать сразу. */
    if (aircraft_db_count() > 0)
        printf("База бортов: загружено %d записей из aircraft_db.csv\n\n", aircraft_db_count());
    else if (aircraft_db_last_warning())
        fprintf(stderr, "%s\n\n", aircraft_db_last_warning());
    if (airport_db_count() > 0)
        printf("База аэропортов: загружено %d записей из airport_db.csv\n\n", airport_db_count());
    else if (airport_db_last_warning())
        fprintf(stderr, "%s\n\n", airport_db_last_warning());

    int did_something = 0;
    int if_mode = 0; /* 0 = автоопределение, 1 = всегда IF, -1 = никогда */
    int verbose = 0;
#ifdef _WIN32
    const char *ar_port = NULL;
    int ar_baud = 9600;
    double ar_freq_mhz = 0.0;
#endif

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            decode_wav_file(argv[++i], if_mode, verbose);
            did_something = 1;
        } else if (strcmp(argv[i], "--if") == 0) {
            if_mode = 1;
        } else if (strcmp(argv[i], "--no-if") == 0 || strcmp(argv[i], "--audio") == 0) {
            if_mode = -1;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
#ifdef _WIN32
        else if (strcmp(argv[i], "--ar5000-port") == 0 && i + 1 < argc) {
            ar_port = argv[++i];
        } else if (strcmp(argv[i], "--ar5000-baud") == 0 && i + 1 < argc) {
            ar_baud = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--ar5000-freq") == 0 && i + 1 < argc) {
            ar_freq_mhz = atof(argv[++i]);
        } else if (strcmp(argv[i], "-l") == 0) {
            int dev = -1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                dev = atoi(argv[++i]);
            }
            /* Если перед -l были указаны параметры AR5000 - подключаемся,
             * настраиваем режим/полосу/частоту и отключаемся после
             * завершения приёма (Ctrl+C). Без --ar5000-port приёмник не
             * трогаем вообще - работаем как раньше, просто со звуковой
             * картой, без управления по RS232. */
            if (ar_port) {
                printf("AR5000: подключаюсь к %s на %d бод...\n", ar_port, ar_baud);
                if (ar5000_connect(ar_port, ar_baud) != 0) {
                    fprintf(stderr, "AR5000: не удалось открыть %s - продолжаю без управления приёмником\n",
                            ar_port);
                } else {
                    char ver[64] = "";
                    if (ar5000_get_version(ver, sizeof(ver)) == 0)
                        printf("AR5000: приёмник ответил, версия %s\n", ver);
                    ar5000_set_mode(1);      /* AM */
                    ar5000_set_bandwidth(2); /* 6 кГц */
                    if (ar_freq_mhz > 0.01) {
                        uint32_t hz = (uint32_t)(ar_freq_mhz * 1000000.0 + 0.5);
                        ar5000_set_frequency_hz(hz);
                        printf("AR5000: настроен на %.3f МГц\n", ar_freq_mhz);
                    }
                }
            }
            run_live_capture(dev, NULL);
            if (ar_port)
                ar5000_disconnect();
            did_something = 1;
        } else if (strcmp(argv[i], "--list-devices") == 0) {
            list_audio_devices();
            did_something = 1;
        }
#endif
        else {
            fprintf(stderr, "Неизвестный параметр: %s\n", argv[i]);
        }
    }

    if (!did_something) {
        print_usage(argv[0]);
        return 1;
    }
    return 0;
}
