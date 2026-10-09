#ifdef _WIN32

#include "ar5000.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HANDLE g_hcom = INVALID_HANDLE_VALUE;

/* Обмен по порту (запись команды + чтение ответа) должен быть атомарным:
 * если поток опроса уровня сигнала и поток сканера частот (gui_win.c)
 * одновременно пошлют свои команды, их ответы могут перемешаться и
 * сломать протокол. CRITICAL_SECTION гарантирует, что весь цикл
 * "команда -> ответ" одного вызова полностью завершается, прежде чем
 * начнётся следующий, из какого бы потока он ни пришёл. */
static CRITICAL_SECTION g_io_cs;

void ar5000_module_init(void) {
    InitializeCriticalSection(&g_io_cs);
}

/* ---------- низкоуровневый обмен по порту ---------- */

static int send_only(const char *cmd) {
    if (g_hcom == INVALID_HANDLE_VALUE)
        return -1;
    char outbuf[64];
    int n = snprintf(outbuf, sizeof(outbuf), "%s\r", cmd);
    if (n <= 0)
        return -1;
    DWORD written = 0;
    if (!WriteFile(g_hcom, outbuf, (DWORD)n, &written, NULL) || written != (DWORD)n)
        return -1;
    return 0;
}

/* Отправляет команду и построчно читает ответ (до <CR>/<LF> или
 * тайм-аута - см. SetCommTimeouts в ar5000_connect). Ведущие пустые
 * строки (просто <CR><LF> без данных) пропускаются - некоторые версии
 * прошивки дублируют символ конца строки. */
static int send_and_read(const char *cmd, char *response, size_t response_size) {
    if (send_only(cmd) != 0)
        return -1;
    if (!response || response_size == 0)
        return 0;

    size_t pos = 0;
    response[0] = '\0';
    for (int i = 0; i < 512; i++) {
        char c;
        DWORD nread = 0;
        if (!ReadFile(g_hcom, &c, 1, &nread, NULL))
            return -1;
        if (nread == 0)
            break; /* тайм-аут - приёмник не ответил (или ответ короче ожидаемого) */
        if (c == '\r' || c == '\n') {
            if (pos > 0)
                break;
            continue;
        }
        if (pos < response_size - 1)
            response[pos++] = c;
    }
    response[pos] = '\0';
    return pos > 0 ? 0 : -1;
}

/* Обёртки с блокировкой - все ПУБЛИЧНЫЕ функции модуля (ar5000_set_...,
 * ar5000_get_...) идут ТОЛЬКО через них, а не напрямую через send_only/
 * send_and_read, чтобы гарантировать атомарность каждого обмена. */
static int locked_send_only(const char *cmd) {
    EnterCriticalSection(&g_io_cs);
    int rc = send_only(cmd);
    LeaveCriticalSection(&g_io_cs);
    return rc;
}

static int locked_send_and_read(const char *cmd, char *response, size_t response_size) {
    EnterCriticalSection(&g_io_cs);
    int rc = send_and_read(cmd, response, response_size);
    LeaveCriticalSection(&g_io_cs);
    return rc;
}

/* ---------- подключение ---------- */

int ar5000_connect(const char *com_port, int baud_rate) {
    ar5000_disconnect(); /* если уже было открыто соединение - сначала аккуратно закрыть */

    /* Префикс "\\.\\" (устройство, а не просто имя) нужен, чтобы
     * корректно открывались порты COM10 и выше - без него Windows
     * понимает только COM1-COM9. */
    char path[32];
    snprintf(path, sizeof(path), "\\\\.\\%s", com_port);

    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                            OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return -1;

    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        CloseHandle(h);
        return -1;
    }

    /* Параметры порта строго по протоколу AR5000: 8 бит данных, 2
     * стоп-бита, без чётности, программное управление потоком
     * XON/XOFF (не аппаратное RTS/CTS!). Должны совпадать с тем, что
     * выставлено в CONFIG-меню самого приёмника (там же настраивается
     * скорость - 4800/9600/19200). */
    dcb.BaudRate = (DWORD)baud_rate;
    dcb.ByteSize = 8;
    dcb.StopBits = TWOSTOPBITS;
    dcb.Parity = NOPARITY;
    dcb.fParity = FALSE;
    dcb.fOutX = TRUE;
    dcb.fInX = TRUE;
    dcb.XonChar = 0x11;  /* DC1 */
    dcb.XoffChar = 0x13; /* DC3 */
    dcb.XonLim = 100;
    dcb.XoffLim = 100;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fBinary = TRUE;

    if (!SetCommState(h, &dcb)) {
        CloseHandle(h);
        return -1;
    }

    /* Тайм-ауты чтения: приёмник обычно отвечает быстро (десятки мс),
     * но не мгновенно - даём разумный запас, не подвешивая GUI
     * надолго, если приёмник вдруг не ответит (например, выключен или
     * ещё не переключился в REMOTE-режим). */
    COMMTIMEOUTS timeouts;
    memset(&timeouts, 0, sizeof(timeouts));
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 400;
    timeouts.ReadTotalTimeoutMultiplier = 5;
    timeouts.WriteTotalTimeoutConstant = 300;
    timeouts.WriteTotalTimeoutMultiplier = 5;
    SetCommTimeouts(h, &timeouts);

    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    g_hcom = h;
    return 0;
}

void ar5000_disconnect(void) {
    if (g_hcom != INVALID_HANDLE_VALUE) {
        /* EX - выход из режима удалённого управления: без этого
         * приёмник может остаться недоступен с передней панели до
         * следующего включения питания. */
        locked_send_only("EX");
        CloseHandle(g_hcom);
        g_hcom = INVALID_HANDLE_VALUE;
    }
}

int ar5000_is_connected(void) {
    return g_hcom != INVALID_HANDLE_VALUE;
}

/* ---------- команды ---------- */

int ar5000_set_frequency_hz(uint32_t hz) {
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "RF%010u", (unsigned)hz);
    return locked_send_only(cmd);
}

int ar5000_get_frequency_hz(uint32_t *out_hz) {
    char resp[32];
    if (locked_send_and_read("RF", resp, sizeof(resp)) != 0)
        return -1;
    if (strncmp(resp, "RF", 2) != 0)
        return -1;
    *out_hz = (uint32_t)strtoul(resp + 2, NULL, 10);
    return 0;
}

int ar5000_set_mode(int mode) {
    char cmd[8];
    snprintf(cmd, sizeof(cmd), "MD%d", mode);
    return locked_send_only(cmd);
}

int ar5000_set_bandwidth(int bw) {
    char cmd[8];
    snprintf(cmd, sizeof(cmd), "BW%d", bw);
    return locked_send_only(cmd);
}

int ar5000_set_attenuator(int att) {
    char cmd[8];
    snprintf(cmd, sizeof(cmd), "AT%d", att);
    return locked_send_only(cmd);
}

int ar5000_get_signal_level(int *out_level, int *out_squelch_open) {
    char resp[32];
    if (locked_send_and_read("LM", resp, sizeof(resp)) != 0)
        return -1;
    if (strncmp(resp, "LM", 2) != 0)
        return -1;

    const char *p = resp + 2;
    int squelch_open = 1;
    for (const char *q = p; *q; q++) {
        if (*q == '%') {
            squelch_open = 0;
            break;
        }
    }

    size_t len = strlen(p);
    if (len < 2)
        return -1;
    char hexbuf[3] = {p[len - 2], p[len - 1], '\0'};
    char *endptr = NULL;
    long level = strtol(hexbuf, &endptr, 16);
    if (endptr == hexbuf)
        return -1; /* ответ не похож на ожидаемый формат */

    *out_level = (int)level;
    *out_squelch_open = squelch_open;
    return 0;
}

int ar5000_get_version(char *buf, size_t buflen) {
    char resp[64];
    if (locked_send_and_read("VR", resp, sizeof(resp)) != 0)
        return -1;
    if (buf && buflen > 0) {
        strncpy(buf, resp, buflen - 1);
        buf[buflen - 1] = '\0';
    }
    return 0;
}

/* ---------- каналы памяти ---------- */

/* Читает ОДИН блок из 10 каналов памяти начиная с bank*100+start_channel
 * (команда "MA" + 1 цифра банка + 2 цифры начального канала, например
 * "MA120" - банк 1, каналы 20-29 - именно так подписан пример ответа в
 * документации: MX120...MX129, где "1" - банк, "20".."29" - номер
 * канала). Каждая строка ответа - либо "MXmnn ---" (канал пуст), либо
 * "MXmnn MP0 GA0 RF<частота> ST<шаг> AU<n> MD<режим> TM<комментарий>"
 * (поля кроме MX и RF не гарантированно в этом порядке/присутствуют -
 * ищем по названию поля, а не по фиксированной позиции). Заблокировано
 * целиком (EnterCriticalSection/Leave...) - это единственное место в
 * модуле, где ОДНА команда порождает НЕСКОЛЬКО строк ответа, поэтому
 * не может идти через locked_send_and_read (тот читает только одну
 * строку) - блокируем вручную на всё время чтения блока. */
static const char *skip_sp(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Разбор одной строки ответа MA: "MXmnn ---" или "MXmnn MP0 GA0 RF0088300000 ST100000 AU1 MD0 TMBBC".
 * Разбираем по словам (код из двух букв + значение), а не поиском подстрок: так "MD" внутри комментария
 * не принимается за режим. Комментарий (TM) идёт последним и может содержать пробелы. */
static void parse_memory_line(const char *line, Ar5000MemoryEntry *e) {
    memset(e, 0, sizeof(*e));
    e->bank = -1;
    e->channel = -1;
    e->mode = -1;
    e->pass = -1;
    e->bw = -1;
    e->att = -1;
    snprintf(e->raw, sizeof(e->raw), "%.127s", line); /* длинную строку обрезаем, не обрывая молча */
    if (strncmp(line, "MX", 2) != 0 || strlen(line) < 5 || line[2] < '0' || line[2] > '9' ||
        line[3] < '0' || line[3] > '9' || line[4] < '0' || line[4] > '9')
        return;
    e->bank = line[2] - '0';
    e->channel = (line[3] - '0') * 10 + (line[4] - '0');
    const char *p = skip_sp(line + 5);
    if (strncmp(p, "---", 3) == 0)
        return; /* пустой канал */
    e->occupied = 1;
    while (*p) {
        p = skip_sp(p);
        if (!*p) break;
        const char *q = p;
        while (*q && *q != ' ' && *q != '\t') q++;
        size_t len = (size_t)(q - p);
        if (len >= 2) {
            char code[3] = {p[0], p[1], 0};
            char val[24];
            size_t vl = len - 2 < sizeof val - 1 ? len - 2 : sizeof val - 1;
            memcpy(val, p + 2, vl);
            val[vl] = 0;
            if (!strcmp(code, "TM")) {
                strncpy(e->comment, p + 2, sizeof(e->comment) - 1); /* всё до конца строки */
                size_t n = strlen(e->comment);
                while (n && (e->comment[n - 1] == ' ' || e->comment[n - 1] == '\t')) e->comment[--n] = 0;
                break;
            }
            if (!strcmp(code, "RF")) e->freq_hz = (uint32_t)strtoul(val, NULL, 10);
            else if (!strcmp(code, "MD")) e->mode = atoi(val);
            else if (!strcmp(code, "MP")) e->pass = atoi(val);
            else if (!strcmp(code, "ST")) e->step_hz = (uint32_t)strtoul(val, NULL, 10);
            else if (!strcmp(code, "BW")) e->bw = atoi(val);
            else if (!strcmp(code, "AT")) e->att = atoi(val);
        }
        p = q;
    }
}

int ar5000_read_memory_block(int bank, int start_channel, Ar5000MemoryEntry *out, int max_entries) {
    if (g_hcom == INVALID_HANDLE_VALUE)
        return -1;

    char cmd[16];
    snprintf(cmd, sizeof(cmd), "MA%d%02d", bank, start_channel);

    EnterCriticalSection(&g_io_cs);
    PurgeComm(g_hcom, PURGE_RXCLEAR); /* остатки прежних ответов сдвинули бы строки */
    if (send_only(cmd) != 0) {
        LeaveCriticalSection(&g_io_cs);
        return -1;
    }

    int n = 0;
    while (n < max_entries && n < 10) {
        char line[160];
        size_t pos = 0;
        int got_any = 0;
        for (int i = 0; i < 512; i++) {
            char c;
            DWORD nread = 0;
            if (!ReadFile(g_hcom, &c, 1, &nread, NULL))
                break;
            if (nread == 0)
                break; /* тайм-аут - вероятно, приёмник закончил отвечать */
            if (c == '\r' || c == '\n') {
                if (pos > 0) {
                    got_any = 1;
                    break;
                }
                continue;
            }
            if (pos < sizeof(line) - 1)
                line[pos++] = c;
            got_any = 1;
        }
        line[pos] = '\0';
        if (!got_any)
            break; /* тайм-аут без единого символа - строк больше не будет */
        parse_memory_line(line, &out[n]);
        n++;
    }

    LeaveCriticalSection(&g_io_cs);
    return n;
}

int ar5000_list_memory_bank(int bank, Ar5000MemoryEntry *out, int max_entries) {
    int total = 0;
    for (int block = 0; block < 10 && total < max_entries; block++) {
        int got = ar5000_read_memory_block(bank, block * 10, out + total, max_entries - total);
        if (got <= 0)
            break; /* нет ответа на этот блок - не пытаемся читать оставшиеся,
                     * чтобы не зависать на молчащем приёмнике */
        total += got;
    }
    return total;
}

int ar5000_recall_memory(int bank, int channel) {
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "MR%d%02d", bank, channel);
    return locked_send_only(cmd);
}

#endif /* _WIN32 */
