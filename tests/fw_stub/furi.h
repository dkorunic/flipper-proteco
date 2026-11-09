// Host stand-in for the bits of furi that lib/subghz/protocols/{proteco,common}.c use.
#pragma once
#include <assert.h>
#include <stdlib.h>
#include <string.h>
// furi's malloc hands out zeroed memory and the protocols count on it (encoder.front, ...)
#define malloc(n)      calloc(1, (n))
#define furi_assert(x) assert(x)
#define furi_crash(...) abort()
#define UNUSED(x)       (void)(x)
#define EXT_PATH(x)     "/ext/" x
#define FURI_LOG_E(...)
typedef struct FuriString FuriString;
void furi_string_cat_printf(FuriString* string, const char format[], ...);
