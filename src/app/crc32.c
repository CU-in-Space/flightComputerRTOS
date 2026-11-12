#include "app/crc32.h"

#define POLYNOMIAL  0xEDB88320UL   // reversed form

uint32_t crc32(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint32_t crc = 0xFFFFFFFFUL;

    for (size_t i = 0; i < length; i++) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc & 1U) ? (crc >> 1) ^ POLYNOMIAL : crc >> 1;
        }
    }
    return ~crc;
}
