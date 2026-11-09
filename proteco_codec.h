#pragma once

/*
 * Proteco MOVER gate remote codec, 433.92 MHz AM650, 24 bit fixed code.
 * Same line coding as CAME, but at Te ~440us instead of 320us:
 *   guard  : LOW  ~21*Te (~9.1ms)
 *   start  : HIGH Te
 *   bit 1  : LOW 2*Te, HIGH Te
 *   bit 0  : LOW Te,   HIGH 2*Te
 * MSB first, frame repeated while the button is held.
 * The firmware patch (proteco-mover-subghz.patch) uses the same timings and state machine.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <toolbox/level_duration.h>

#define PROTECO_BITS        24u
#define PROTECO_PROTOCOL    "Proteco"
#define PROTECO_UPLOAD_SIZE (2u + PROTECO_BITS * 2u)
#define PROTECO_TX_REPEAT   10u

typedef struct {
    uint8_t step;
    uint32_t te_last;
    uint32_t data;
    uint8_t bits;
} ProtecoDecoder;

typedef struct {
    LevelDuration upload[PROTECO_UPLOAD_SIZE];
    size_t front;
    size_t repeat;
} ProtecoEncoder;

void proteco_decoder_reset(ProtecoDecoder* decoder);

/** Feed one level/duration pair. Returns true when a complete frame was decoded into *key. */
bool proteco_decoder_feed(ProtecoDecoder* decoder, bool level, uint32_t duration, uint32_t* key);

/** Build the frame for key, to be sent repeat times. */
void proteco_encoder_load(ProtecoEncoder* encoder, uint32_t key, size_t repeat);

/** SubGhzDevice async TX callback, context is a ProtecoEncoder. */
LevelDuration proteco_encoder_yield(void* context);
