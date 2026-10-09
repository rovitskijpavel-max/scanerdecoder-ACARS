#include "cfdiu_fault.h"
#include <string.h>
#include <ctype.h>

int cfdiu_fault_parse(const char *text, CfdiuFault *out) {
    memset(out, 0, sizeof(*out));
    if (!text)
        return 0;

    const char *p = strstr(text, "#CFB");
    if (!p)
        return 0;
    p += 4;

    /* 3-буквенный тип записи (FLR/WRN и т.п.) - должны быть буквы */
    if (!isalpha((unsigned char)p[0]) || !isalpha((unsigned char)p[1]) || !isalpha((unsigned char)p[2]))
        return 0;
    out->severity[0] = p[0];
    out->severity[1] = p[1];
    out->severity[2] = p[2];
    out->severity[3] = '\0';
    p += 3;

    if (*p != '/')
        return 0;
    p++;

    if (!isalpha((unsigned char)p[0]) || !isalpha((unsigned char)p[1]))
        return 0;
    out->code2[0] = p[0];
    out->code2[1] = p[1];
    out->code2[2] = '\0';
    p += 2;

    /* 20 цифр подряд - см. комментарий в заголовке про то, что именно
     * известно про этот блок (мало) */
    int ndigits = 0;
    while (isdigit((unsigned char)p[ndigits]))
        ndigits++;
    if (ndigits < 15 || ndigits > 22)
        return 0; /* заметно отличается от всех наблюдавшихся примеров -
                   * не похоже на этот формат, не гадаем */
    int copy_len = ndigits < (int)sizeof(out->timestamp_code) - 1
                       ? ndigits
                       : (int)sizeof(out->timestamp_code) - 1;
    memcpy(out->timestamp_code, p, (size_t)copy_len);
    out->timestamp_code[copy_len] = '\0';
    p += ndigits;

    /* остаток строки (до конца текста или перевода строки) -
     * человекочитаемое описание */
    size_t desc_len = strcspn(p, "\r\n");
    if (desc_len == 0)
        return 0; /* нет описания - не похоже на настоящий отчёт */
    size_t n = desc_len < sizeof(out->description) - 1 ? desc_len : sizeof(out->description) - 1;
    memcpy(out->description, p, n);
    out->description[n] = '\0';

    out->valid = 1;
    return 1;
}
