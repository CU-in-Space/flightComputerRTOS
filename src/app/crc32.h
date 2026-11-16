#ifndef APP_CRC32_H
#define APP_CRC32_H

#include <stddef.h>
#include <stdint.h>

// The standard CRC-32, the same as Python's zlib.crc32.
uint32_t crc32(const void *data, size_t length);

#endif /* APP_CRC32_H */
