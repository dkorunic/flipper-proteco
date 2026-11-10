// Host check:  make -C tests  [FW=/path/to/firmware]  [RAW="a.sub b.sub"]  (RAW defaults to ../dumps)
// Runs two Proteco implementations side by side: the app's proteco_codec.c and the firmware's
// lib/subghz/protocols/proteco.c from the patch. Both decoders get every signal, and must agree.
//  1. keys round trip through both encoders into both decoders, also with timing jitter
//  2. each .sub given (RAW or BinRAW) is decoded, and its key is re-encoded by the firmware
//     encoder, which must decode again and equal every decoded captured frame at Te resolution.
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../proteco_codec.h"
#include <protocols/common.h>
#include <protocols/proteco.h>
#include <blocks/custom_btn.h>

/* ----------------------- stubs for the firmware code ----------------------- */

struct FlipperFormat {
    uint64_t key;
    uint32_t bits;
};

SubGhzBlockGenericGlobal subghz_block_generic_global;

bool subghz_block_generic_global_button_override_get(uint8_t* button) {
    UNUSED(button);
    return false; // no button editor on the host
}

SubGhzProtocolStatus subghz_block_generic_deserialize_check_count_bit(
    SubGhzBlockGeneric* instance,
    FlipperFormat* ff,
    uint16_t count_bit) {
    if(ff->bits != count_bit) return SubGhzProtocolStatusErrorValueBitCount;
    instance->data_count_bit = ff->bits;
    instance->data = ff->key;
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus subghz_block_generic_serialize(
    SubGhzBlockGeneric* instance,
    FlipperFormat* ff,
    SubGhzRadioPreset* preset) {
    UNUSED(instance);
    UNUSED(ff);
    UNUSED(preset);
    return SubGhzProtocolStatusError;
}

bool flipper_format_rewind(FlipperFormat* ff) {
    UNUSED(ff);
    return true;
}

bool flipper_format_read_uint32(FlipperFormat* ff, const char* key, uint32_t* data, uint16_t n) {
    UNUSED(ff);
    UNUSED(key);
    UNUSED(data);
    UNUSED(n);
    return false; // no Repeat, keep the protocol's default
}

bool flipper_format_write_uint32(
    FlipperFormat* ff,
    const char* key,
    const uint32_t* data,
    uint16_t n) {
    UNUSED(ff);
    UNUSED(key);
    UNUSED(data);
    UNUSED(n);
    return false;
}

bool flipper_format_insert_or_update_hex(
    FlipperFormat* ff,
    const char* key,
    const uint8_t* data,
    uint16_t n) {
    UNUSED(ff);
    UNUSED(key);
    UNUSED(data);
    UNUSED(n);
    return false;
}

void furi_string_cat_printf(FuriString* string, const char format[], ...) {
    UNUSED(string);
    UNUSED(format);
}

/* ----------------------- both decoders on one signal ----------------------- */

typedef struct {
    ProtecoDecoder app;
    SubGhzProtocolDecoderCommon* fw;
    int app_frames, fw_frames;
    uint32_t app_key, fw_key;
    bool app_mixed, fw_mixed; // more than one key seen
} Sink;

static void sink_fw_callback(SubGhzProtocolDecoderBase* base, void* context) {
    Sink* sink = context;
    uint32_t key = ((SubGhzProtocolDecoderCommon*)base)->generic.data;
    assert(((SubGhzProtocolDecoderCommon*)base)->generic.data_count_bit == PROTECO_BITS);
    if(sink->fw_frames++ && key != sink->fw_key) sink->fw_mixed = true;
    sink->fw_key = key;
}

static void sink_init(Sink* sink) {
    memset(sink, 0, sizeof(*sink));
    sink->fw = subghz_protocol_proteco.decoder->alloc(NULL);
    sink->fw->base.callback = sink_fw_callback;
    sink->fw->base.context = sink;
    subghz_protocol_proteco.decoder->reset(sink->fw);
}

static void sink_feed(void* context, bool level, uint32_t duration) {
    Sink* sink = context;
    uint32_t key;
    if(proteco_decoder_feed(&sink->app, level, duration, &key)) {
        if(sink->app_frames++ && key != sink->app_key) sink->app_mixed = true;
        sink->app_key = key;
    }
    subghz_protocol_proteco.decoder->feed(sink->fw, level, duration);
}

/** Closing guard so the last frame is reported, then check both decoders agree. */
static int sink_done(Sink* sink) {
    sink_feed(sink, false, 100000);
    subghz_protocol_proteco.decoder->free(sink->fw);
    assert(sink->app_frames == sink->fw_frames);
    assert(!sink->app_mixed && !sink->fw_mixed);
    if(sink->app_frames) assert(sink->app_key == sink->fw_key);
    return sink->app_frames;
}

/* --------------------------------- encoders -------------------------------- */

static int jitter_of(int jitter) {
    return jitter ? (rand() % (2 * jitter + 1)) - jitter : 0;
}

static int run_app_encoder(uint32_t key, int jitter) {
    ProtecoEncoder enc;
    Sink sink;
    sink_init(&sink);
    proteco_encoder_load(&enc, key, 3);
    for(LevelDuration ld = proteco_encoder_yield(&enc); !level_duration_is_reset(ld);
        ld = proteco_encoder_yield(&enc)) {
        sink_feed(
            &sink,
            level_duration_get_level(ld),
            level_duration_get_duration(ld) + jitter_of(jitter));
    }
    int frames = sink_done(&sink);
    assert(sink.app_key == key);
    // the measured clock and guard of the last frame, exact without jitter
    if(!jitter) assert(sink.app.frame_te == 440 && sink.app.frame_guard == 9020);
    return frames;
}

/** Firmware encoder for key. Fills bits ('0'/'1' per 440us Te, from the start bit) if given. */
static int run_fw_encoder(uint32_t key, int jitter, char* bits, size_t bits_size) {
    FlipperFormat ff = {.key = key, .bits = PROTECO_BITS};
    void* enc = subghz_protocol_proteco.encoder->alloc(NULL);
    assert(subghz_protocol_proteco.encoder->deserialize(enc, &ff) == SubGhzProtocolStatusOk);

    Sink sink;
    sink_init(&sink);
    size_t n = 0, len = 0;
    for(LevelDuration ld = subghz_protocol_proteco.encoder->yield(enc);
        !level_duration_is_reset(ld);
        ld = subghz_protocol_proteco.encoder->yield(enc), n++) {
        bool level = level_duration_get_level(ld);
        uint32_t duration = level_duration_get_duration(ld);
        // first frame: guard, start bit, 24 bit pairs
        // the guard: 21 Te in the first patch, 20.5 Te (the fob's) since PR #1210 was merged
        if(n == 0) assert(!level && duration >= 20 * 440 && duration <= 21 * 440);
        if(bits && n >= 1 && n < PROTECO_UPLOAD_SIZE) {
            assert(duration % 440 == 0);
            for(uint32_t i = 0; i < duration / 440 && len + 1 < bits_size; i++)
                bits[len++] = level ? '1' : '0';
        }
        sink_feed(&sink, level, duration + jitter_of(jitter));
    }
    if(bits) bits[len] = '\0';
    subghz_protocol_proteco.encoder->free(enc);

    int frames = sink_done(&sink);
    assert(sink.fw_key == key);
    return frames;
}

/* ---------------------------------- files ---------------------------------- */

/** Append n copies of c to the growing '0'/'1' capture string. */
static void capture_add(char** buf, size_t* len, size_t* size, char c, size_t n) {
    if(*len + n + 1 > *size) {
        *size = (*len + n + 1) * 2;
        *buf = realloc(*buf, *size);
        assert(*buf);
    }
    memset(*buf + *len, c, n);
    *len += n;
    (*buf)[*len] = '\0';
}

/** Captured frames identical to encoded, behind a guard the decoder takes: one LOW of
 * 18..24 Te (its window is 7665..10815us). Longer LOWs are silence, not a Proteco guard. */
static int count_frames(const char* capture, const char* encoded) {
    int count = 0;
    for(const char* at = strstr(capture, encoded); at; at = strstr(at + 1, encoded)) {
        const char* run = at;
        while(run > capture && run[-1] == '0')
            run--;
        long guard = at - run;
        if(guard >= 18 && guard <= 24) count++;
    }
    return count;
}

static void decode_file(const char* path) {
    FILE* f = fopen(path, "r");
    if(!f) {
        perror(path);
        exit(1);
    }
    static char line[16384];
    // the signal at Te resolution, '0'/'1' per Te: BinRAW bits as stored, RAW durations
    // rounded to whole 440us Te (the receiver's HIGH stretch / LOW shrink stays well inside)
    char* capture = NULL;
    size_t capture_len = 0, capture_size = 0;
    uint32_t te = 0, chunk_bits = 0;
    bool bin_raw = false;
    Sink sink;
    sink_init(&sink);
    ProtecoBinRaw bin = {.callback = sink_feed, .context = &sink};

    while(fgets(line, sizeof(line), f)) {
        if(!strncmp(line, "Protocol: BinRAW", 16)) bin_raw = true;
        if(!strncmp(line, "TE:", 3)) te = strtoul(line + 3, NULL, 10);
        if(!strncmp(line, "Bit_RAW:", 8)) chunk_bits = strtoul(line + 8, NULL, 10);
        if(!strncmp(line, "Data_RAW:", 9)) {
            assert(bin_raw && te && chunk_bits && chunk_bits <= 8 * 512);
            uint8_t data[512];
            size_t bytes = 0;
            for(char* tok = strtok(line + 9, " \r\n"); tok; tok = strtok(NULL, " \r\n"))
                data[bytes++] = strtoul(tok, NULL, 16);
            assert(bytes == (chunk_bits + 7) / 8);
            proteco_bin_raw_feed(&bin, data, chunk_bits, te);
            for(size_t i = bytes * 8 - chunk_bits; i < bytes * 8; i++) {
                char bit = (data[i / 8] >> (7 - i % 8)) & 1 ? '1' : '0';
                capture_add(&capture, &capture_len, &capture_size, bit, 1);
            }
        }
        if(!strncmp(line, "RAW_Data:", 9)) {
            for(char* tok = strtok(line + 9, " \r\n"); tok; tok = strtok(NULL, " \r\n")) {
                long v = strtol(tok, NULL, 10);
                sink_feed(&sink, v > 0, (uint32_t)labs(v));
                // at least one Te, so a noise spike still splits a LOW, as it does for the decoder
                size_t n = (labs(v) + 220) / 440;
                n = n < 1 ? 1 : (n > 64 ? 64 : n);
                capture_add(&capture, &capture_len, &capture_size, v > 0 ? '1' : '0', n);
            }
        }
    }
    fclose(f);
    proteco_bin_raw_flush(&bin);
    int frames = sink_done(&sink);
    if(!frames) {
        printf("%s: no Proteco frame\n", path);
        exit(1);
    }

    // the firmware encoder must give back what the remote sent: every frame the decoder took
    // is, at Te resolution, exactly the encoder's frame
    static char encoded[PROTECO_UPLOAD_SIZE * 2 + 1];
    assert(run_fw_encoder(sink.fw_key, 0, encoded, sizeof(encoded)) == 10);
    int matched = count_frames(capture ? capture : "", encoded);
    free(capture);
    if(matched != frames) {
        printf("%s: decoded %d frames, but %d match the encoder\n", path, frames, matched);
        exit(1);
    }
    printf(
        "%-40s %-6s %3d frame(s) key 0x%06X, encoder frame == %d/%d captured frames\n",
        path,
        bin_raw ? "BinRAW" : "RAW",
        frames,
        (unsigned)sink.fw_key,
        matched,
        frames);
}

/** Custom buttons (PR #1210): a decoded A, sent as each of the 4 buttons. Skipped on trees
 * whose proteco.c has no custom buttons. */
static void check_custom_buttons(void) {
    // TX3 A, then what the arrows give from A: B, C and the unverified fourth code
    const uint32_t sent[] = {0xDF2396, 0xDF2394, 0xDF2395, 0xDF2397};
    FlipperFormat ff = {.key = 0xDF2396, .bits = PROTECO_BITS};

    subghz_custom_btns_reset();
    void* dec = subghz_protocol_proteco.decoder->alloc(NULL);
    assert(subghz_protocol_proteco.decoder->deserialize(dec, &ff) == SubGhzProtocolStatusOk);
    subghz_protocol_proteco.decoder->get_string(dec, NULL); // the info screen sets the buttons up
    subghz_protocol_proteco.decoder->free(dec);
    if(!subghz_custom_btn_is_allowed()) {
        printf("custom buttons: not in this firmware, skipped\n");
        return;
    }
    assert(subghz_custom_btn_get_original() == 1); // A is button 1

    for(uint8_t id = 0; id < 4; id++) {
        assert(subghz_custom_btn_set(id));
        void* enc = subghz_protocol_proteco.encoder->alloc(NULL);
        assert(subghz_protocol_proteco.encoder->deserialize(enc, &ff) == SubGhzProtocolStatusOk);
        Sink sink;
        sink_init(&sink);
        for(LevelDuration ld = subghz_protocol_proteco.encoder->yield(enc);
            !level_duration_is_reset(ld);
            ld = subghz_protocol_proteco.encoder->yield(enc))
            sink_feed(&sink, level_duration_get_level(ld), level_duration_get_duration(ld));
        subghz_protocol_proteco.encoder->free(enc);
        assert(sink_done(&sink) == 10 && sink.fw_key == sent[id]);
        // the app's button helper steps through the same keys, in the same order
        assert(proteco_button_step(sent[0], id) == sent[id]);
        assert(proteco_button(sent[id]) == "ABCD"[id]);
    }
    assert(!subghz_custom_btn_set(4)); // only 3 other buttons
    subghz_custom_btns_reset(); // the encoder would otherwise keep applying button A
    printf(
        "custom buttons: A -> A, B, C, 4th = 0x%06X 0x%06X 0x%06X 0x%06X: ok\n",
        (unsigned)sent[0],
        (unsigned)sent[1],
        (unsigned)sent[2],
        (unsigned)sent[3]);
}

static int fuzz_fw_frames;
static uint32_t fuzz_fw_key;

static void fuzz_fw_callback(SubGhzProtocolDecoderBase* base, void* context) {
    UNUSED(context);
    fuzz_fw_frames++;
    fuzz_fw_key = ((SubGhzProtocolDecoderCommon*)base)->generic.data;
}

/** App and firmware decoders see the same random signal and must report the same frames,
 * sample for sample: Proteco frames at random clocks, guards and jitter, mixed with noise. */
static void check_decoder_parity(void) {
    ProtecoDecoder app = {0};
    SubGhzProtocolDecoderCommon* fw = subghz_protocol_proteco.decoder->alloc(NULL);
    fw->base.callback = fuzz_fw_callback;
    subghz_protocol_proteco.decoder->reset(fw);
    srand(1210);
    int frames = 0;
    for(int i = 0; i < 20000; i++) {
        uint32_t samples[2 + PROTECO_BITS * 2];
        size_t n = 0;
        if(rand() % 4) { // a frame, possibly broken by its jitter
            uint32_t key = ((uint32_t)rand() << 8 ^ (uint32_t)rand()) & 0xFFFFFF;
            uint32_t te = 360 + rand() % 160, jitter = rand() % 160;
            samples[n++] = te * (8 + rand() % 42) / 2; // guard 4..24.5 Te, across both limits
            samples[n++] = te;
            for(int b = PROTECO_BITS - 1; b >= 0; b--) {
                bool one = (key >> b) & 1;
                samples[n++] = one ? 2 * te : te;
                samples[n++] = one ? te : 2 * te;
            }
            for(size_t k = 0; k < n; k++)
                samples[k] = samples[k] + rand() % (jitter + 1) - jitter / 2;
        } else { // noise
            for(; n < 10; n++)
                samples[n] = 30 + rand() % 12000;
        }
        for(size_t k = 0; k < n; k++) {
            bool level = k % 2; // frames and noise both start LOW
            uint32_t key = 0;
            int before = fuzz_fw_frames;
            bool found = proteco_decoder_feed(&app, level, samples[k], &key);
            subghz_protocol_proteco.decoder->feed(fw, level, samples[k]);
            assert(found == (fuzz_fw_frames == before + 1) && fuzz_fw_frames <= before + 1);
            if(found) {
                assert(key == fuzz_fw_key);
                // same measured clock as the firmware's guard check
                assert(app.frame_te * 37 / 2 <= app.frame_guard);
                assert(app.frame_guard <= app.frame_te * 45 / 2);
                frames++;
            }
        }
        if(n % 2) { // keep levels alternating: pad an odd burst with a short HIGH
            uint32_t key;
            int before = fuzz_fw_frames;
            assert(!proteco_decoder_feed(&app, true, 100, &key));
            subghz_protocol_proteco.decoder->feed(fw, true, 100);
            assert(fuzz_fw_frames == before);
        }
    }
    subghz_protocol_proteco.decoder->free(fw);
    assert(frames > 1000); // the stream really exercised both paths
    printf("decoder parity (app == firmware), %d frames in random signal: ok\n", frames);
}

static void check_button_helpers(void) {
    assert(proteco_button(0xDF2396) == 'A' && proteco_button(0xDF2394) == 'B');
    assert(proteco_button(0xDF2395) == 'C' && proteco_button(0xDF2397) == 'D');
    assert(proteco_button_step(0xDF2396, -1) == 0xDF2397); // A back to D
    assert(proteco_button_step(0xDF2397, 1) == 0xDF2396); // D on to A
    for(uint32_t key = 0; key < 4; key++)
        assert(proteco_button_step(proteco_button_step(key, 1), -1) == key);
}

int main(int argc, char** argv) {
    const uint32_t keys[] = {0x000000, 0xFFFFFF, 0x5AC3E1, 0x123456, 0xA5A5A5, 0xDF2396};
    for(size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        for(int jitter = 0; jitter <= 120; jitter += 120) {
            assert(run_app_encoder(keys[i], jitter) == 3);
            assert(run_fw_encoder(keys[i], jitter, NULL, 0) == 10);
        }
    }

    // a 25th bit (longer, non Proteco frame) must not be reported
    ProtecoEncoder enc;
    Sink sink;
    sink_init(&sink);
    proteco_encoder_load(&enc, 0x123456, 1);
    for(size_t i = 0; i < PROTECO_UPLOAD_SIZE; i++)
        sink_feed(
            &sink,
            level_duration_get_level(enc.upload[i]),
            level_duration_get_duration(enc.upload[i]));
    sink_feed(&sink, false, 440);
    sink_feed(&sink, true, 880);
    assert(sink_done(&sink) == 0);

    // BinRAW chunks: right aligned, runs merged across chunk boundaries
    Sink bin_sink;
    sink_init(&bin_sink);
    ProtecoBinRaw bin = {.callback = sink_feed, .context = &bin_sink};
    const uint8_t a[] = {0x00, 0x00, 0x01, 0x25, 0x92},
                  b[] = {0x49, 0x6C, 0xB6, 0xC9, 0x2D, 0x96, 0x4B};
    proteco_bin_raw_feed(&bin, a, 37, 440); // 3 pad bits, ends inside a run that b continues
    proteco_bin_raw_feed(&bin, b, 56, 440);
    proteco_bin_raw_flush(&bin);
    assert(sink_done(&bin_sink) == 1 && bin_sink.app_key == 0xDF2396);
    printf("codec round trip (app + firmware): ok\n");
    check_button_helpers();
    check_decoder_parity();
    check_custom_buttons();

    for(int i = 1; i < argc; i++)
        decode_file(argv[i]);
    return 0;
}
