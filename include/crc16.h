#ifndef CRC16_H
#define CRC16_H

#include <stdint.h>
#include <stddef.h>

/* CRC-16/KERMIT (используется в ACARS как BCS - Block Check Sequence).
 * Полином 0x1021 в "отражённой" форме 0x8408, начальное значение 0x0000,
 * биты подаются младшим битом вперёд (что соответствует порядку передачи
 * символов ACARS). Если пропустить через эту функцию все байты кадра,
 * включая два байта BCS в конце, результат должен быть равен 0 -
 * это и есть проверка целостности сообщения. */

static inline uint16_t crc16_update(uint16_t crc, uint8_t byte) {
    crc ^= (uint16_t)byte;
    for (int i = 0; i < 8; i++) {
        if (crc & 1)
            crc = (crc >> 1) ^ 0x8408;
        else
            crc = (crc >> 1);
    }
    return crc;
}

static inline uint16_t crc16_buffer(const uint8_t *data, size_t len) {
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++)
        crc = crc16_update(crc, data[i]);
    return crc;
}

#endif
