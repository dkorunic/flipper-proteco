// Host stand-in: the test defines struct FlipperFormat as {key, bits}.
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct FlipperFormat FlipperFormat;
bool flipper_format_rewind(FlipperFormat* ff);
bool flipper_format_read_uint32(FlipperFormat* ff, const char* key, uint32_t* data, uint16_t count);
bool flipper_format_write_uint32(FlipperFormat* ff, const char* key, const uint32_t* data, uint16_t count);
bool flipper_format_insert_or_update_hex(FlipperFormat* ff, const char* key, const uint8_t* data, uint16_t count);
