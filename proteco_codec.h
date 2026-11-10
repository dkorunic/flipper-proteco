#pragma once

/*
 * Proteco MOVER gate remote codec, 433.92 MHz AM650, 24 bit fixed code.
 * Same line coding as CAME, but at Te ~440us instead of 320us:
 *   guard  : LOW  20.5*Te (~9.0ms)
 *   start  : HIGH Te
 *   bit 1  : LOW 2*Te, HIGH Te
 *   bit 0  : LOW Te,   HIGH 2*Te
 * MSB first, frame repeated while the button is held. The low 2 bits are the button:
 * A = 10, B = 00, C = 01 on a Proteco TX3, 11 is the unverified fourth code (D).
 * The guard is checked against the frame's own Te (18.5..22.5 Te), measured over its
 * 73 Te of pulses. The firmware decoder (lib/subghz/protocols/proteco.c, Unleashed PR
 * #1210) uses the same timings and state machine.
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
    uint32_t te; // sum of the frame's pulses so far
    uint32_t guard; // the LOW that opened the frame
    // of the last frame reported: its mean Te and opening guard, us
    uint32_t frame_te;
    uint32_t frame_guard;
} ProtecoDecoder;

typedef struct {
    LevelDuration upload[PROTECO_UPLOAD_SIZE];
    size_t front;
    size_t repeat;
} ProtecoEncoder;

void proteco_decoder_reset(ProtecoDecoder* decoder);

/** Feed one level/duration pair. Returns true when a complete frame was decoded into *key,
 * with its measured Te and guard in decoder->frame_te / frame_guard. */
bool proteco_decoder_feed(ProtecoDecoder* decoder, bool level, uint32_t duration, uint32_t* key);

/** Build the frame for key, to be sent repeat times. */
void proteco_encoder_load(ProtecoEncoder* encoder, uint32_t key, size_t repeat);

/** SubGhzDevice async TX callback, context is a ProtecoEncoder. */
LevelDuration proteco_encoder_yield(void* context);

/** The key's button: 'A', 'B', 'C', or 'D' for the unverified fourth code. */
char proteco_button(uint32_t key);

/** The key with its button moved step places on in A, B, C, D order (step +1 or -1, wraps).
 * Same order as the firmware's custom buttons (arrow keys in the stock Sub-GHz app). */
uint32_t proteco_button_step(uint32_t key, int step);

typedef void (*ProtecoPairCallback)(void* context, bool level, uint32_t duration);

/** Turns BinRAW bit chunks back into level/duration pairs. */
typedef struct {
    ProtecoPairCallback callback;
    void* context;
    bool level;
    uint32_t duration; // of the run not yet passed on, 0 = none
} ProtecoBinRaw;

/**
 * Feed one Bit_RAW/Data_RAW chunk: bits right aligned in data (as bin_raw.c stores them),
 * MSB first, 1 = HIGH, each bit te us. A run that goes on into the next chunk is merged,
 * so call proteco_bin_raw_flush() after the last chunk.
 */
void proteco_bin_raw_feed(ProtecoBinRaw* bin, const uint8_t* data, size_t bits, uint32_t te);

/** Pass on the last pending run. */
void proteco_bin_raw_flush(ProtecoBinRaw* bin);
