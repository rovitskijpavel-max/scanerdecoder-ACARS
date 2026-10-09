#ifndef UNITS_H
#define UNITS_H

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#define FEET_TO_METERS 0.3048

/* Высота из текста сообщения (футы, как передал борт) -> "10970 м (36000 ft)".
 * Если строка не число (например, прочерки) - возвращается как есть с "ft". Возвращает 1,
 * если перевод выполнен, и тогда *ft_out (если не NULL) - высота в футах. */
static inline int alt_ft_text(const char *alt, char *out, size_t cap, int *ft_out) {
    const char *p = alt ? alt : "";
    while (*p == ' ') p++;
    const char *q = p;
    if (*q == '-') q++;
    const char *d = q;
    while (isdigit((unsigned char)*q)) q++;
    const char *e = q;
    while (*e == ' ' || *e == '\r' || *e == '\n') e++;
    if (q == d || *e != '\0' || q - d > 7) {
        snprintf(out, cap, "%.30s%s", p, p[0] ? " ft" : "");
        return 0;
    }
    long ft = strtol(p, NULL, 10);
    long m = (long)(ft * FEET_TO_METERS + (ft >= 0 ? 0.5 : -0.5));
    snprintf(out, cap, "%ld м (%ld ft)", m, ft);
    if (ft_out) *ft_out = (int)ft;
    return 1;
}

/* Ветер из полей WDIR/WSPD: направление - число, читается как сотые доли градуса (33359 -> 333.59°), скорость - узлы.
 * Результат: "из 333.6° (ССЗ), 75 уз (139 км/ч, 39 м/с)". Если что-то не число - пишем как есть.
 * Подтверждено только по косвенным признакам (направления 290-360° и скорость растёт с высотой: 5 уз у земли,
 * 75-85 уз на 35000 ft) - документации на формат нет. */
static inline void wind_text(const char *dir, const char *spd, char *out, size_t cap) {
    static const char *const pt[16] = {"С", "ССВ", "СВ", "ВСВ", "В", "ВЮВ", "ЮВ", "ЮЮВ",
                                       "Ю", "ЮЮЗ", "ЮЗ", "ЗЮЗ", "З", "ЗСЗ", "СЗ", "ССЗ"};
    char d[24] = "", v[24] = "";
    int nd = 0, nv = 0;
    for (const char *q = dir ? dir : ""; *q && nd < 20; q++) if (*q != ' ' && *q != '\r' && *q != '\n') d[nd++] = *q;
    for (const char *q = spd ? spd : ""; *q && nv < 20; q++) if (*q != ' ' && *q != '\r' && *q != '\n') v[nv++] = *q;
    int dn = nd > 0, vn = nv > 0;
    for (int i = 0; i < nd; i++) if (!isdigit((unsigned char)d[i])) dn = 0;
    for (int i = 0; i < nv; i++) if (!isdigit((unsigned char)v[i])) vn = 0;
    char a[80] = "направление неизвестно", b[80] = "скорость неизвестна";
    if (dn) {
        double deg = atoi(d) / 100.0;
        int k = (int)((deg + 11.25) / 22.5) % 16;
        snprintf(a, sizeof a, "из %.1f° (%s)", deg, pt[k < 0 ? 0 : k]);
    } else if (nd) snprintf(a, sizeof a, "направление %.20s", d);
    if (vn) {
        int kt = atoi(v);
        snprintf(b, sizeof b, "%d уз (%.0f км/ч, %.0f м/с)", kt, kt * 1.852, kt * 0.514444);
    } else if (nv) snprintf(b, sizeof b, "скорость %.20s", v);
    snprintf(out, cap, "%.70s, %.70s", a, b);
}

#endif
