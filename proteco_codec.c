#include "proteco_codec.h"

#define TE_SHORT        440u
#define TE_LONG         880u
#define TE_DELTA        150u
// guard in half Te, as in the firmware: the fob's gap is 20.4-20.6 Te, not a whole number
#define GUARD_TE_X2     41u
#define GUARD_TE_X2_MIN 37u
#define GUARD_TE_X2_MAX 45u
#define FRAME_TE        (1u + PROTECO_BITS * 3u) // start bit + 3 Te per bit

#define DIFF(a, b)  (((a) < (b)) ? ((b) - (a)) : ((a) - (b)))
#define IS_SHORT(d) (DIFF(d, TE_SHORT) < TE_DELTA)
#define IS_LONG(d)  (DIFF(d, TE_LONG) < TE_DELTA)
#define IS_GAP(d)   ((d) >= TE_SHORT * 4)

enum {
    StepReset = 0,
    StepFoundStartBit,
    StepSaveDuration,
    StepCheckDuration,
};

void proteco_decoder_reset(ProtecoDecoder* decoder) {
    decoder->step = StepReset;
}

/** Is the LOW that opened the frame 18.5..22.5 Te of the frame's own clock? */
static bool proteco_guard_ok(const ProtecoDecoder* decoder) {
    uint32_t te = decoder->te / FRAME_TE;
    return decoder->guard >= te * GUARD_TE_X2_MIN / 2 &&
           decoder->guard <= te * GUARD_TE_X2_MAX / 2;
}

bool proteco_decoder_feed(ProtecoDecoder* decoder, bool level, uint32_t duration, uint32_t* key) {
    bool found = false;

    switch(decoder->step) {
    case StepReset:
        if(!level && IS_GAP(duration)) {
            decoder->guard = duration;
            decoder->step = StepFoundStartBit;
        }
        break;
    case StepFoundStartBit:
        if(level && IS_SHORT(duration)) {
            decoder->data = 0;
            decoder->bits = 0;
            decoder->te = duration;
            decoder->step = StepSaveDuration;
        } else {
            decoder->step = StepReset;
        }
        break;
    case StepSaveDuration:
        if(level) {
            decoder->step = StepReset;
        } else if(IS_GAP(duration)) {
            // this gap ends the frame and is the next frame's guard
            if(decoder->bits == PROTECO_BITS && proteco_guard_ok(decoder)) {
                *key = decoder->data;
                decoder->frame_te = decoder->te / FRAME_TE;
                decoder->frame_guard = decoder->guard;
                found = true;
            }
            decoder->guard = duration;
            decoder->step = StepFoundStartBit;
        } else {
            decoder->te_last = duration;
            decoder->te += duration;
            decoder->step = StepCheckDuration;
        }
        break;
    case StepCheckDuration:
        if(level && decoder->bits < PROTECO_BITS && IS_SHORT(decoder->te_last) &&
           IS_LONG(duration)) {
            decoder->data <<= 1;
            decoder->bits++;
            decoder->te += duration;
            decoder->step = StepSaveDuration;
        } else if(
            level && decoder->bits < PROTECO_BITS && IS_LONG(decoder->te_last) &&
            IS_SHORT(duration)) {
            decoder->data = (decoder->data << 1) | 1;
            decoder->bits++;
            decoder->te += duration;
            decoder->step = StepSaveDuration;
        } else {
            decoder->step = StepReset;
        }
        break;
    }
    return found;
}

void proteco_encoder_load(ProtecoEncoder* encoder, uint32_t key, size_t repeat) {
    size_t i = 0;
    encoder->upload[i++] = level_duration_make(false, TE_SHORT * GUARD_TE_X2 / 2);
    encoder->upload[i++] = level_duration_make(true, TE_SHORT);
    for(int bit = PROTECO_BITS - 1; bit >= 0; bit--) {
        bool one = (key >> bit) & 1;
        encoder->upload[i++] = level_duration_make(false, one ? TE_LONG : TE_SHORT);
        encoder->upload[i++] = level_duration_make(true, one ? TE_SHORT : TE_LONG);
    }
    encoder->front = 0;
    encoder->repeat = repeat;
}

// low 2 bits of the key, in A, B, C, D order; and the other way round
static const uint8_t proteco_button_code[4] = {0x2, 0x0, 0x1, 0x3};
static const uint8_t proteco_button_index[4] = {1, 2, 0, 3};

char proteco_button(uint32_t key) {
    return (char)('A' + proteco_button_index[key & 0x3]);
}

uint32_t proteco_button_step(uint32_t key, int step) {
    uint8_t index = (uint8_t)((proteco_button_index[key & 0x3] + 4 + step) & 0x3);
    return (key & ~(uint32_t)0x3) | proteco_button_code[index];
}

LevelDuration proteco_encoder_yield(void* context) {
    ProtecoEncoder* encoder = context;
    if(encoder->repeat == 0) return level_duration_reset();

    LevelDuration ret = encoder->upload[encoder->front];
    if(++encoder->front == PROTECO_UPLOAD_SIZE) {
        encoder->front = 0;
        encoder->repeat--;
    }
    return ret;
}

void proteco_bin_raw_feed(ProtecoBinRaw* bin, const uint8_t* data, size_t bits, uint32_t te) {
    size_t pad = (8 - bits % 8) % 8;
    for(size_t i = pad; i < pad + bits; i++) {
        bool level = (data[i / 8] >> (7 - i % 8)) & 1;
        if(bin->duration && level != bin->level) proteco_bin_raw_flush(bin);
        bin->level = level;
        bin->duration += te;
    }
}

void proteco_bin_raw_flush(ProtecoBinRaw* bin) {
    if(bin->duration) bin->callback(bin->context, bin->level, bin->duration);
    bin->duration = 0;
}
