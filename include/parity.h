#ifndef PARITY_H
#define PARITY_H

#include <stdint.h>

/* ACARS использует символы 7 бит ASCII + 1 бит нечётного паритета (odd
 * parity): количество единичных битов во всём 8-битном байте (данные +
 * бит паритета) должно быть нечётным. */
static inline int parity_is_odd(uint8_t b) {
    int c = 0;
    while (b) {
        c ^= (b & 1);
        b >>= 1;
    }
    return c; /* 1 если количество единичных бит нечётное */
}

#endif
