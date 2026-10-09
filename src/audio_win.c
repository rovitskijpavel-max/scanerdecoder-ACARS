#ifdef _WIN32

/*
 * Живой приём звука со звуковой карты и декодирование ACARS "на лету".
 *
 * Используется классическое Win32 Multimedia API - функции waveIn*
 * (winmm.dll). Это НАМЕРЕННЫЙ выбор вместо более современного WASAPI:
 * waveIn* работает на ВСЕХ версиях Windows от 95/98/2000/XP до 10/11,
 * тогда как WASAPI появился только в Vista и на XP отсутствует
 * физически (не просто "не рекомендуется" - там нет самих системных
 * DLL с этими функциями, программа откажется запускаться). Один .exe,
 * собранный этим кодом, работает одинаково и на XP, и на новых
 * системах - разделять сборку под разные версии Windows не нужно.
 *
 * ВАЖНО: этот файл собирается только под Windows (MSVC или MinGW-w64).
 * Он не тестировался сборкой в этой сессии (здесь нет Windows-тулчейна) -
 * логика написана строго по документированному Win32 Multimedia API, но
 * при первом запуске возможны мелкие шероховатости (см. README, раздел
 * "Отладка живого приёма"). Часть с демодуляцией/разбором кадров уже
 * многократно проверена отдельно на WAV-файлах (в т.ч. на реальных
 * записях) и работает корректно.
 *
 * Подключение: выход AR5000 (лучше "discriminator out" / детекторный
 * выход, если он выведен на заднюю панель, а не AF OUT после AGC) -> вход
 * звуковой карты (Line In или Mic In). Если у приёмника только обычный
 * AF-выход - тоже подойдёт, но избегайте автоматической регулировки
 * громкости/шумоподавления на входе звуковой карты в настройках Windows.
 *
 * Демодуляция идёт через mskpll.c (согласованный фильтр + Гарднер-ФАПЧ) -
 * тот же основной, проверенный на реальных сигналах конвейер, что и в
 * режиме декодирования WAV-файлов (main.c). Он передискретизирует поток
 * до "родной" для алгоритма частоты 12500 Гц и работает непрерывно, без
 * необходимости делить эфир на всплески - что естественно ложится на
 * живой, непрерывный поток из звуковой карты.
 */

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "framesync.h"
#include "labels.h"
#include "wavio.h"
#include "mskpll.h"
#include "audio_win.h"
#include "history_log.h"
#include "msg_sink.h"
#include "arinc622.h"
#include "ooi_parse.h"
#include "aircraft_db.h"
#include "airport_db.h"
#include "cfdiu_fault.h"
#include "asu_telemetry.h"
#include "metar.h"

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif
/* Под MinGW winmm подключается через -lwinmm в Makefile/CMakeLists.txt. */

/* Кандидаты частоты дискретизации по убыванию предпочтения - пробуем
 * открыть устройство на каждой по очереди, пока одна не подойдёт.
 * 48000/44100 - то, что умеет практически любая карта 2000-х и новее;
 * 22050/11025/8000 - подстраховка для совсем старого/дешёвого железа
 * (актуально именно для XP-эпохи). */
static const int kSampleRateCandidates[] = {48000, 44100, 22050, 11025, 8000, 32000, 16000, 96000};
#define N_RATE_CANDIDATES (int)(sizeof(kSampleRateCandidates) / sizeof(kSampleRateCandidates[0]))

#define NUM_BUFFERS 8
#define BUFFER_MS 100

typedef struct {
    FrameSync fs;
} LiveCtx;

static void live_on_bit(int bit, void *ctx) {
    framesync_feed_bit(&((LiveCtx *)ctx)->fs, bit);
}

static void print_airport_field(const char *label, const char *icao) {
    AirportInfo ap;
    if (airport_db_lookup(icao, &ap))
        printf("%s%s  (%s, %s)\n", label, icao, ap.name, ap.municipality);
    else
        printf("%s%s\n", label, icao);
}

static void print_message_live(const AcarsMessage *m) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    printf("---------------------------------------------\n");
    printf("[%02d:%02d:%02d] CRC: %s%s\n", st.wHour, st.wMinute, st.wSecond,
           m->crc_ok ? "OK" : "ОШИБКА",
           m->crc_fixed ? (m->bits_fixed == 1 ? " (исправлен 1 бит)" : " (исправлено 2 бита)") : "");
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
    if (m->label[0])
        printf("  Label    : %s  (%s)\n", m->label, acars_label_lookup(m->label));
    if (m->block_id)
        printf("  Block id : %c\n", m->block_id);
    printf("  Suffix   : %s\n", m->is_etb ? "ETB (продолжение следует)" : "ETX");
    if (m->has_text) {
        printf("  Text     :\n%s\n", m->text);

        Arinc622Envelope env;
        if (arinc622_parse(m->text, m->addr, &env)) {
            printf("  --- Конверт ARINC 622 (ADS-C/CPDLC) ---\n");
            printf("  Наземная станция : %s\n", env.ground_station);
            printf("  Тип (IMI)        : %s  (%s)\n", env.imi, env.imi_description);
            printf("  Борт             : %s\n", env.aircraft_reg);
            printf("  Внутр. CRC       : %s\n", env.inner_crc);
            printf("  Данные (hex, %d байт): %s\n", env.hex_payload_len / 2, env.hex_payload);
        }

        OoiInfo ooi;
        if (ooi_parse(m->text, &ooi)) {
            printf("  --- Маршрут (формат OOOI FI) ---\n");
            if (ooi.flight_id[0])
                printf("  Рейс             : %s\n", ooi.flight_id);
            if (ooi.departure[0])
                print_airport_field("  Вылет из         : ", ooi.departure);
            if (ooi.destination[0])
                print_airport_field("  Назначение       : ", ooi.destination);
            if (ooi.arrival[0])
                print_airport_field("  Аэропорт (ARR)   : ", ooi.arrival);
        } else {
            AirportInfo ap1, ap2;
            if (airport_db_scan_pair(m->text, &ap1, &ap2)) {
                printf("  --- В тексте замечены подряд идущие коды аэропортов ---\n");
                printf("  %s - %s, %s\n", ap1.icao, ap1.name, ap1.municipality);
                printf("  %s - %s, %s\n", ap2.icao, ap2.name, ap2.municipality);
                printf("  (порядок вылет/назначение неизвестен - формат не задокументирован)\n");
            }
        }

        CfdiuFault fault;
        if (cfdiu_fault_parse(m->text, &fault)) {
            printf("  --- Отчёт CFDIU (%s) ---\n",
                   strcmp(fault.severity, "WRN") == 0 ? "предупреждение" : "отказ");
            printf("  %s\n", fault.description);
        }

        AsuTelemetry tel;
        if (asu_telemetry_parse(m->text, &tel)) {
            printf("  --- Телеметрия (формат ASU/AFL) ---\n");
            if (tel.fuel[0])
                printf("  Топливо          : %s\n", tel.fuel);
            if (tel.wind_dir[0] || tel.wind_speed[0])
                printf("  Ветер            : напр. %s, скорость %s\n", tel.wind_dir, tel.wind_speed);
            if (tel.lat[0] || tel.lon[0])
                printf("  Координаты       : %s, %s\n", tel.lat, tel.lon);
            if (tel.eta[0])
                printf("  ETA              : %s\n", tel.eta);
            if (tel.alt[0])
                printf("  Высота           : %s\n", tel.alt);
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
    fflush(stdout);
}

static void live_on_msg(const AcarsMessage *m, void *ctx) {
    (void)ctx;
    if (!m->crc_ok)
        return; /* в живом режиме показываем только достоверные сообщения */
    print_message_live(m);
    history_log_message(m, "live");
    msg_sink_notify(m, "live");
}

int list_audio_devices(void) {
    UINT n = waveInGetNumDevs();
    printf("Устройства записи звука (всего %u):\n", n);
    if (n == 0) {
        printf("  (не найдено ни одного устройства записи)\n");
        return 0;
    }
    for (UINT i = 0; i < n; i++) {
        char name[256];
        if (get_audio_device_name((int)i, name, sizeof(name)) == 0)
            printf("  [%u] %s\n", i, name);
        else
            printf("  [%u] (не удалось получить имя устройства)\n", i);
    }
    return 0;
}

int get_audio_device_count(void) {
    return (int)waveInGetNumDevs();
}

int get_audio_device_name(int index, char *buf, int buflen) {
    WAVEINCAPSA caps;
    if (waveInGetDevCapsA((UINT)index, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
        return -1;
    strncpy(buf, caps.szPname, (size_t)buflen - 1);
    buf[buflen - 1] = '\0';
    return 0;
}

/* Пытается открыть устройство подряд на нескольких частотах
 * дискретизации (см. kSampleRateCandidates), сначала в МОНО (нужно
 * для остального конвейера), а если НИ ОДНА частота в моно не
 * подошла - повторяет тот же перебор в СТЕРЕО: некоторые звуковые
 * карты (особенно вход AUX/Line In, заведённый через гнездо
 * микрофона) поддерживают захват только в 2 канала. Если удалось
 * открыть только в стерео - *out_channels станет 2, и вызывающий код
 * обязан свести каналы в моно сам (усреднением), см. run_live_capture().
 *
 * ВАЖНО: настоящие коды ошибок драйвера сохраняются в err_summary
 * (какие коды встретились и сколько раз). Раньше при неудаче функция
 * просто возвращала константу "формат не поддерживается", что могло
 * маскировать реальную причину (устройство занято, нет доступа,
 * нет драйвера...) - и пользователь искал не там. */
static MMRESULT open_device(UINT device_id, HWAVEIN *out_hwi,
                             int *out_rate, int *out_channels,
                             char *err_summary, size_t err_summary_size) {
    MMRESULT codes[8];
    int counts[8];
    int n_codes = 0;
    MMRESULT first_error = MMSYSERR_NOERROR;

    for (int ch = 1; ch <= 2; ch++) {
        for (int i = 0; i < N_RATE_CANDIDATES; i++) {
            WAVEFORMATEX fmt;
            memset(&fmt, 0, sizeof(fmt));
            fmt.wFormatTag = WAVE_FORMAT_PCM;
            fmt.nChannels = (WORD)ch;
            fmt.nSamplesPerSec = (DWORD)kSampleRateCandidates[i];
            fmt.wBitsPerSample = 16;
            fmt.nBlockAlign = (WORD)(fmt.nChannels * fmt.wBitsPerSample / 8);
            fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
            fmt.cbSize = 0;

            /* CALLBACK_NULL - никакого события/окна/потока в качестве
             * уведомителя не требуется вовсе, только опрос WHDR_DONE
             * самим вызывающим кодом (см. run_live_capture) - так надёжнее:
             * CALLBACK_EVENT добавляет ещё один параметр (сам HANDLE),
             * который теоретически тоже может быть отвергнут MMSYSERR_
             * INVALPARAM на некоторых сочетаниях драйвер/аудиостек, а
             * CALLBACK_NULL исключает этот параметр из уравнения целиком. */
            MMRESULT mr = waveInOpen(out_hwi, device_id, &fmt, 0, 0, CALLBACK_NULL);
            if (mr == MMSYSERR_NOERROR) {
                *out_rate = kSampleRateCandidates[i];
                *out_channels = ch;
                if (err_summary && err_summary_size)
                    err_summary[0] = '\0';
                return MMSYSERR_NOERROR;
            }

            if (first_error == MMSYSERR_NOERROR)
                first_error = mr;
            int found = 0;
            for (int k = 0; k < n_codes; k++) {
                if (codes[k] == mr) {
                    counts[k]++;
                    found = 1;
                    break;
                }
            }
            if (!found && n_codes < 8) {
                codes[n_codes] = mr;
                counts[n_codes] = 1;
                n_codes++;
            }
        }
    }

    if (err_summary && err_summary_size) {
        size_t pos = 0;
        err_summary[0] = '\0';
        for (int k = 0; k < n_codes; k++) {
            char text[160];
            waveInGetErrorTextA(codes[k], text, sizeof(text));
            int n = snprintf(err_summary + pos, err_summary_size - pos,
                             "  код %u: %s  (%d попыток)\n",
                             (unsigned)codes[k], text, counts[k]);
            if (n < 0 || (size_t)n >= err_summary_size - pos)
                break;
            pos += (size_t)n;
        }
    }
    return first_error;
}

/* Печатает, что само устройство сообщает о своих возможностях
 * (имя, число каналов, поддерживаемые стандартные форматы) - чтобы
 * при отказе было видно, врёт ли устройство про поддержку или
 * проблема не в формате вовсе. */
/* caps.szPname приходит от драйвера в СИСТЕМНОЙ ANSI-кодовой странице
 * (не UTF-8) - если напечатать её как есть через fprintf, а дальше по
 * цепочке (консоль в режиме UTF-8, либо труба в окно GUI) байты
 * интерпретируются как UTF-8, кириллица и другие не-ASCII символы
 * превращаются в кракозябры/квадратики. Перекодируем через страницу
 * Windows -> Unicode -> UTF-8, тем же способом, что и в gui_win.c для
 * списка устройств (ansi_to_wide там же). */
static void ansi_to_utf8(const char *ansi, char *out, size_t out_size) {
    wchar_t wide[256];
    int wn = MultiByteToWideChar(CP_ACP, 0, ansi, -1, wide, 256);
    if (wn <= 0) {
        if (out_size) out[0] = '\0';
        return;
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)out_size, NULL, NULL);
    if (n <= 0 && out_size)
        out[0] = '\0';
}

static void print_device_caps(UINT device_id) {
    WAVEINCAPSA caps;
    memset(&caps, 0, sizeof(caps));
    MMRESULT mr = waveInGetDevCapsA(device_id, &caps, sizeof(caps));
    if (mr != MMSYSERR_NOERROR) {
        char text[160];
        waveInGetErrorTextA(mr, text, sizeof(text));
        fprintf(stderr, "  (не удалось даже запросить возможности устройства: %s)\n", text);
        return;
    }
    char name_utf8[512];
    ansi_to_utf8(caps.szPname, name_utf8, sizeof(name_utf8));
    fprintf(stderr, "  Устройство сообщает о себе: \"%s\", каналов: %u, форматы (маска): 0x%08lX\n",
            name_utf8, (unsigned)caps.wChannels, (unsigned long)caps.dwFormats);
    if (caps.dwFormats == 0)
        fprintf(stderr, "  (маска форматов = 0 - драйвер не заявляет ни одного стандартного\n"
                        "   формата: обычно так бывает, когда устройство отключено, заблокировано\n"
                        "   настройками конфиденциальности Windows или им владеет другая программа)\n");
}

/* stop_flag - если не NULL, цикл захвата периодически проверяет его и
 * корректно завершается, как только кто-то (например, поток GUI)
 * выставит его в ненулевое значение. Если NULL - ведёт себя как раньше,
 * работает "вечно" (для консольного режима -l, где выход - Ctrl+C). */
int run_live_capture(int device_index, volatile int *stop_flag) {
    UINT device_id = (device_index < 0) ? WAVE_MAPPER : (UINT)device_index;

    HWAVEIN hwi = NULL;
    int samplerate = 0;
    int channels = 1;
    char err_summary[1024];
    MMRESULT mr = open_device(device_id, &hwi, &samplerate, &channels,
                               err_summary, sizeof(err_summary));
    int used_default_mapper = 0;
    if (mr != MMSYSERR_NOERROR && device_id != WAVE_MAPPER) {
        /* Последняя попытка: устройство по умолчанию через системный
         * "маппер" (он сам подбирает устройство и при необходимости
         * конвертирует формат). Если сработает - значит, выбранный
         * индекс устройства проблемный, а система в целом записывать
         * умеет. */
        char summary2[1024];
        HWAVEIN hwi2 = NULL;
        int rate2 = 0, ch2 = 1;
        MMRESULT mr2 = open_device(WAVE_MAPPER, &hwi2, &rate2, &ch2,
                                    summary2, sizeof(summary2));
        if (mr2 == MMSYSERR_NOERROR) {
            hwi = hwi2;
            samplerate = rate2;
            channels = ch2;
            mr = MMSYSERR_NOERROR;
            used_default_mapper = 1;
        }
    }
    if (mr != MMSYSERR_NOERROR) {
        char errtext[256];
        waveInGetErrorTextA(mr, errtext, sizeof(errtext));
        fprintf(stderr, "Не удалось открыть устройство записи №%d: %s\n"
                        "Что именно ответил драйвер на все попытки (моно/стерео, %d частот):\n%s",
                device_index, errtext, N_RATE_CANDIDATES, err_summary);
        print_device_caps(device_id);
        fprintf(stderr,
                "Что проверить:\n"
                "  1) Параметры Windows -> Конфиденциальность -> Микрофон: включить\n"
                "     \"Доступ к микрофону\" и \"Разрешить классическим приложениям\n"
                "     доступ к микрофону\".\n"
                "  2) Панель управления -> Звук -> Запись: устройство должно быть\n"
                "     включено (не серое), Свойства -> Дополнительно -> формат по\n"
                "     умолчанию, например \"2 канала, 16 бит, 48000 Гц\"; там же снять\n"
                "     галочки \"Разрешить приложениям использовать устройство в\n"
                "     монопольном режиме\".\n"
                "  3) Закрыть другие программы, которые могут держать микрофон\n"
                "     (мессенджеры, запись экрана, Realtek-панель и т.п.).\n"
                "  4) Кнопка \"Диагностика\" покажет список устройств; если их несколько -\n"
                "     выберите другое в выпадающем списке.\n");
        return -1;
    }
    if (used_default_mapper)
        printf("(выбранное устройство №%d не открылось - использовано устройство по умолчанию)\n",
               device_index);

    printf("Захват: устройство %s%d, %d Гц, 16 бит, %s\n",
           (device_index < 0) ? "по умолчанию" : "№", device_index, samplerate,
           channels == 2 ? "стерео (сводится в моно)" : "моно");
    printf("Ожидание ACARS-сообщений... (Ctrl+C для выхода)\n\n");

    int buf_samples = samplerate * BUFFER_MS / 1000; /* в кадрах (frames), не в int16 */
    size_t buf_bytes = (size_t)buf_samples * (size_t)channels * sizeof(int16_t);

    WAVEHDR headers[NUM_BUFFERS];
    memset(headers, 0, sizeof(headers));
    for (int i = 0; i < NUM_BUFFERS; i++) {
        headers[i].lpData = (LPSTR)malloc(buf_bytes);
        headers[i].dwBufferLength = (DWORD)buf_bytes;
        waveInPrepareHeader(hwi, &headers[i], sizeof(WAVEHDR));
        waveInAddBuffer(hwi, &headers[i], sizeof(WAVEHDR));
    }

    waveInStart(hwi);

    LiveCtx ctx;
    framesync_init(&ctx.fs, live_on_msg, NULL);
    MskPll pll;
    mskpll_init(&pll, live_on_bit, &ctx);

    float *if_buf = NULL;
    size_t if_buf_cap = 0;

    while (1) {
        if (stop_flag && *stop_flag) {
            printf("Приём остановлен пользователем.\n");
            fflush(stdout);
            break;
        }

        int any_done = 0;
        for (int i = 0; i < NUM_BUFFERS; i++) {
            if (!(headers[i].dwFlags & WHDR_DONE))
                continue;
            any_done = 1;

            UINT32 n_int16 = headers[i].dwBytesRecorded / sizeof(int16_t);
            if (n_int16 > 0) {
                int16_t *sp = (int16_t *)headers[i].lpData;

                /* int16 -> float [-1..1]; если захват в стерео (см.
                 * open_device) - сначала сводим L+R в моно усреднением,
                 * иначе дальнейший конвейер (ожидающий один сэмпл на
                 * отсчёт времени) получил бы чередующиеся L/R отсчёты
                 * как будто это подряд идущие МОНО-отсчёты - вдвое
                 * "быстрее" настоящего сигнала и полностью нечитаемо. */
                static float mono_buf[192000];
                UINT32 n_frames = (channels == 2) ? (n_int16 / 2) : n_int16;
                UINT32 nn = n_frames;
                if (nn > 192000) nn = 192000;
                if (channels == 2) {
                    for (UINT32 f = 0; f < nn; f++) {
                        float l = sp[f * 2] / 32768.0f;
                        float r = sp[f * 2 + 1] / 32768.0f;
                        mono_buf[f] = (l + r) * 0.5f;
                    }
                } else {
                    for (UINT32 f = 0; f < nn; f++)
                        mono_buf[f] = sp[f] / 32768.0f;
                }

                size_t rcount = 0;
                if (samplerate != MSKPLL_IF_RATE_HZ) {
                    float *tmp = NULL;
                    resample_linear(mono_buf, nn, samplerate, MSKPLL_IF_RATE_HZ, &tmp, &rcount);
                    if (rcount > if_buf_cap) {
                        free(if_buf);
                        if_buf = (float *)malloc(rcount * sizeof(float));
                        if_buf_cap = rcount;
                    }
                    memcpy(if_buf, tmp, rcount * sizeof(float));
                    free(tmp);
                } else {
                    if (nn > if_buf_cap) {
                        free(if_buf);
                        if_buf = (float *)malloc(nn * sizeof(float));
                        if_buf_cap = nn;
                    }
                    memcpy(if_buf, mono_buf, nn * sizeof(float));
                    rcount = nn;
                }

                /* mskpll_process копит состояние (ФАПЧ, кольцевой буфер
                 * согласованного фильтра) между вызовами, так что дробить
                 * поток на куски по 100 мс - абсолютно безопасно и не
                 * портит синхронизацию; framesync.c аналогично хранит
                 * состояние между вызовами framesync_feed_bit. */
                mskpll_process(&pll, if_buf, (int)rcount);
            }

            waveInAddBuffer(hwi, &headers[i], sizeof(WAVEHDR));
        }
        if (!any_done)
            Sleep(20); /* небольшая пауза, чтобы не грузить процессор пустым опросом */
    }

    waveInStop(hwi);
    for (int i = 0; i < NUM_BUFFERS; i++) {
        waveInUnprepareHeader(hwi, &headers[i], sizeof(WAVEHDR));
        free(headers[i].lpData);
    }
    waveInClose(hwi);
    free(if_buf);
    return 0;
}

#endif /* _WIN32 */
