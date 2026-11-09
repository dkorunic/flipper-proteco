#include "proteco_codec.h"

#define TE_SHORT 440u
#define TE_LONG  880u
#define TE_DELTA 150u
#define GUARD_TE 21u

#define DIFF(a, b)  (((a) < (b)) ? ((b) - (a)) : ((a) - (b)))
#define IS_SHORT(d) (DIFF(d, TE_SHORT) < TE_DELTA)
#define IS_LONG(d)  (DIFF(d, TE_LONG) < TE_DELTA)
#define IS_GUARD(d) (DIFF(d, TE_SHORT * GUARD_TE) < TE_DELTA * GUARD_TE / 2)

enum {
    StepReset = 0,
    StepFoundStartBit,
    StepSaveDuration,
    StepCheckDuration,
};

void proteco_decoder_reset(ProtecoDecoder* decoder) {
    decoder->step = StepReset;
}

bool proteco_decoder_feed(ProtecoDecoder* decoder, bool level, uint32_t duration, uint32_t* key) {
    bool found = false;

    switch(decoder->step) {
    case StepReset:
        if(!level && IS_GUARD(duration)) decoder->step = StepFoundStartBit;
        break;
    case StepFoundStartBit:
        if(level && IS_SHORT(duration)) {
            decoder->data = 0;
            decoder->bits = 0;
            decoder->step = StepSaveDuration;
        } else {
            decoder->step = StepReset;
        }
        break;
    case StepSaveDuration:
        if(level) {
            decoder->step = StepReset;
        } else if(duration >= TE_SHORT * 4) {
            // guard reached, the frame ends here and the next one may follow
            if(decoder->bits == PROTECO_BITS) {
                *key = decoder->data;
                found = true;
            }
            decoder->step = IS_GUARD(duration) ? StepFoundStartBit : StepReset;
        } else {
            decoder->te_last = duration;
            decoder->step = StepCheckDuration;
        }
        break;
    case StepCheckDuration:
        if(level && decoder->bits < PROTECO_BITS && IS_SHORT(decoder->te_last) &&
           IS_LONG(duration)) {
            decoder->data <<= 1;
            decoder->bits++;
            decoder->step = StepSaveDuration;
        } else if(
            level && decoder->bits < PROTECO_BITS && IS_LONG(decoder->te_last) &&
            IS_SHORT(duration)) {
            decoder->data = (decoder->data << 1) | 1;
            decoder->bits++;
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
    encoder->upload[i++] = level_duration_make(false, TE_SHORT * GUARD_TE);
    encoder->upload[i++] = level_duration_make(true, TE_SHORT);
    for(int bit = PROTECO_BITS - 1; bit >= 0; bit--) {
        bool one = (key >> bit) & 1;
        encoder->upload[i++] = level_duration_make(false, one ? TE_LONG : TE_SHORT);
        encoder->upload[i++] = level_duration_make(true, one ? TE_SHORT : TE_LONG);
    }
    encoder->front = 0;
    encoder->repeat = repeat;
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
